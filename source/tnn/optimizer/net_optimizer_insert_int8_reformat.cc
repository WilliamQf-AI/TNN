// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/optimizer/net_optimizer_insert_int8_reformat.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "tnn/core/layer_type.h"
#include "tnn/core/macro.h"
#include "tnn/interpreter/layer_param.h"
#include "tnn/interpreter/tnn/objseri.h"
#include "tnn/optimizer/net_optimizer_manager.h"
#include "tnn/optimizer/optimizer_const.h"

namespace TNN_NS {

namespace optimizer {

    // Plast priority: reformat after all fuse
    NetOptimizerRegister<NetOptimizerInsertInt8Reformat> g_net_optimizer_insert_int8_reformat(OptPriority::P2);
    static const std::string REFORMAT_NAME_SUFFIX = "_int8_reformat";

    std::string NetOptimizerInsertInt8Reformat::Strategy() {
        return kNetOptimizerInsertInt8Reformat;
    }

    bool NetOptimizerInsertInt8Reformat::IsSupported(const NetworkConfig &net_config) {
        auto device = net_config.device_type;
        device_     = GetDevice(device);
        return device == DEVICE_ARM || device == DEVICE_NAIVE || device == DEVICE_X86;
    }

    std::shared_ptr<LayerInfo> NetOptimizerInsertInt8Reformat::CreateReformat(std::string name, bool src_quantized) {
        std::shared_ptr<LayerInfo> new_layer = std::shared_ptr<LayerInfo>(new LayerInfo());
        new_layer->type                      = LAYER_REFORMAT;
        new_layer->type_str                  = "Reformat";
        new_layer->name                      = name;
        ReformatLayerParam *param            = new ReformatLayerParam();
        new_layer->param                     = std::shared_ptr<LayerParam>(param);
        new_layer->param->type               = new_layer->type_str;
        new_layer->param->name               = new_layer->name;
        // only define quant/dequant here, layout after layer init
        param->src_type = src_quantized ? DATA_TYPE_INT8 : DATA_TYPE_FLOAT;
        param->dst_type = src_quantized ? DATA_TYPE_FLOAT : DATA_TYPE_INT8;
        if (device_->GetDeviceType() == DEVICE_ARM) {
            param->src_format = src_quantized ? DATA_FORMAT_NHWC4 : DATA_FORMAT_NC4HW4;
            param->dst_format = src_quantized ? DATA_FORMAT_NC4HW4 : DATA_FORMAT_NHWC4;
        }
        return new_layer;
    }

    Status NetOptimizerInsertInt8Reformat::Optimize(NetStructure *structure, NetResource *resource) {
        if (!structure) {
            LOGE("Error: empty NetStructure\n");
            return Status(TNNERR_NET_ERR, "Error: empty NetStructure");
        }

        std::vector<std::shared_ptr<LayerInfo>> layers_orig = structure->layers;
        const int count                                     = (const int)layers_orig.size();
        if (count <= 1) {
            return TNN_OK;
        }

        // only insert reformat before quantized layer now
        auto is_quantized_net = GetQuantizedInfoFromNetStructure(structure);
        if (!is_quantized_net) {
            return TNN_OK;
        }

        std::vector<std::shared_ptr<LayerInfo>> layers_fused;

        // if model input is used for multiple layers with different data types,
        // reformat layers are inserted at beginning.
        // support multi inputs/outputs.
        for (const auto &iter : structure->inputs_shape_map) {
            const auto &model_input = iter.first;
            LOGD("NetOptimizerInsertInt8Reformat::Optimize, process model input: %s\n", model_input.c_str());
            int need_int8_input = 0;
            int need_fp32_input = 0;
            for (const auto &cur_layer : layers_orig) {
                for (const auto &layer_input : cur_layer->inputs) {
                    if (layer_input == model_input) {
                        if (cur_layer->param->quantized) {
                            ++need_int8_input;
                        } else {
                            ++need_fp32_input;
                        }
                        break;
                    }
                }
            }
            if (need_int8_input > 0 && need_fp32_input > 0) {
                std::vector<std::string> reformat_outs = {model_input};
                // fake input layer act as a quantized layer
                std::shared_ptr<LayerInfo> fake_input_layer = std::make_shared<LayerInfo>();
                fake_input_layer->param                     = std::make_shared<LayerParam>();
                DataType input_data_type                    = structure->input_data_type_map[model_input];
                bool src_quantized;
                if (input_data_type == DATA_TYPE_FLOAT) {
                    fake_input_layer->param->quantized = false;
                    src_quantized                      = false;
                } else if (input_data_type == DATA_TYPE_INT8) {
                    fake_input_layer->param->quantized = true;
                    src_quantized                      = true;
                } else {
                    LOGE("NetOptimizerInsertInt8Reformat::Optimize, get invalid input data type %d\n", input_data_type);
                    return {TNNERR_UNSUPPORT_NET,
                            "NetOptimizerInsertInt8Reformat::Optimize, get invalid input data type"};
                }
                // create int8 -> fp32 or fp32-> int8 reformat layer
                std::string new_layer_name           = model_input + REFORMAT_NAME_SUFFIX + "__from_model_input__";
                std::shared_ptr<LayerInfo> new_layer = CreateReformat(new_layer_name, src_quantized);

                AdjustLayer(layers_orig, structure, resource, fake_input_layer, new_layer, reformat_outs, -1, count);

                LOGD("Insert int8 refomat layer : src %s dst %s\n", new_layer->inputs[0].c_str(),
                     new_layer->outputs[0].c_str());
                layers_fused.push_back(new_layer);
            }
        }

        for (int index = 0; index < count; index++) {
            auto cur_layer = layers_orig[index];
            layers_fused.push_back(cur_layer);
            if (cur_layer->type == LAYER_REFORMAT) {
                continue;
            }

            // find blobs need reformat
            // support multi inputs/outputs
            // only quant & dequant now
            std::vector<std::string> reformat_outs;
            reformat_outs.clear();
            for (auto cur_out : cur_layer->outputs) {
                bool need_reformat = false;
                for (int next_id = index + 1; next_id < count; next_id++) {
                    auto next_layer = layers_orig[next_id];
                    if (next_layer->type == LAYER_REFORMAT) {
                        continue;
                    }
                    for (auto next_in : next_layer->inputs) {
                        if (next_in == cur_out && next_layer->param->quantized != cur_layer->param->quantized) {
                            need_reformat = true;
                        }
                    }
                }
                if (need_reformat) {
                    reformat_outs.push_back(cur_out);
                }
            }
            if (!reformat_outs.size()) {
                continue;
            }

            std::shared_ptr<LayerInfo> new_layer =
                CreateReformat(cur_layer->name + REFORMAT_NAME_SUFFIX, cur_layer->param->quantized);

            AdjustLayer(layers_orig, structure, resource, cur_layer, new_layer, reformat_outs, index, count);

            LOGD("Insert int8 refomat layer: src %s dst %s\n", new_layer->inputs[0].c_str(),
                 new_layer->outputs[0].c_str());
            layers_fused.push_back(new_layer);
        }
        structure->layers = layers_fused;

        return TNN_OK;
    }

    void NetOptimizerInsertInt8Reformat::AdjustLayer(std::vector<std::shared_ptr<LayerInfo>> &layers_orig,
                                                     NetStructure *structure, NetResource *resource,
                                                     std::shared_ptr<LayerInfo> &cur_layer,
                                                     std::shared_ptr<LayerInfo> &new_layer,
                                                     std::vector<std::string> &cur_layer_outputs, const int index,
                                                     const int count) {
        new_layer->inputs = cur_layer_outputs;
        for (auto cur_out : cur_layer_outputs) {
            auto new_out = cur_out + REFORMAT_NAME_SUFFIX;
            new_layer->outputs.push_back(new_out);
            structure->blobs.insert(new_out);
            // change the inputs of successed int8 layers
            for (int next_id = index + 1; next_id < count; next_id++) {
                auto next_layer = layers_orig[next_id];
                for (auto &next_in : next_layer->inputs) {
                    // only use reformat out when quantized param diff
                    if (next_in == cur_out && next_layer->param->quantized != cur_layer->param->quantized) {
                        next_in = new_out;
                    }
                }
            }
            if (!cur_layer->param->quantized) {
                std::string old_blob_scale_name = cur_out + BLOB_SCALE_SUFFIX;
                std::string new_blob_scale_name = new_out + BLOB_SCALE_SUFFIX;
                auto &resource_map              = resource->resource_map;
                if (resource_map.find(old_blob_scale_name) == resource_map.end()) {
                    LOGE("NetOptimizerInsertInt8Reformat::Optimize can not get %s blob scale\n",
                         old_blob_scale_name.c_str());
                    return;
                }
                auto blob_scale = resource_map[old_blob_scale_name];
                resource_map.insert(std::make_pair(new_blob_scale_name, blob_scale));
            }
        }
    }
}  // namespace optimizer

}  // namespace TNN_NS
