# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
from typing import Union

import numpy as np
import torch
from transformers import (BertConfig, BertPreTrainedModel, RobertaConfig,
                          RobertaPreTrainedModel)

import tensorrt_llm


def extract_layer_idx(name):
    ss = name.split('.')
    for s in ss:
        if s.isdigit():
            return s
    return None


def split(v, tp_size, idx, dim=0):
    if tp_size == 1:
        return v
    if len(v.shape) == 1:
        return np.ascontiguousarray(np.split(v, tp_size)[idx].copy())
    elif len(v.shape) == 2:
        return np.ascontiguousarray(np.split(v, tp_size, axis=dim)[idx].copy())
    return None


def load_from_hf_model(tensorrt_llm_model: tensorrt_llm.module.Module,
                       hf_model: Union[BertPreTrainedModel,
                                       RobertaPreTrainedModel],
                       hf_model_config: Union[BertConfig, RobertaConfig],
                       rank=0,
                       tensor_parallel=1,
                       fp16=False):
    qkv_weight = [[None, None, None]
                  for _ in range(hf_model_config.num_hidden_layers)]

    qkv_bias = [[None, None, None]
                for _ in range(hf_model_config.num_hidden_layers)]

    torch_dtype = torch.float16 if fp16 else torch.float32
    for k, v in hf_model.state_dict().items():
        v = v.to(torch_dtype).cpu().numpy()
        if 'embeddings.word_embeddings.weight' in k:
            tensorrt_llm_model.embedding.vocab_embedding.weight.value = v
        elif 'embeddings.position_embeddings.weight' in k:
            tensorrt_llm_model.embedding.position_embedding.weight.value = v
        elif 'embeddings.token_type_embeddings.weight' in k:
            tensorrt_llm_model.embedding.token_embedding.weight.value = v
        elif 'embeddings.LayerNorm.weight' in k:
            tensorrt_llm_model.embedding.embedding_ln.weight.value = v
        elif 'embeddings.LayerNorm.bias' in k:
            tensorrt_llm_model.embedding.embedding_ln.bias.value = v
        else:
            layer_idx = extract_layer_idx(k)
            if layer_idx is None:
                continue
            idx = int(layer_idx)
            if 'attention.output.dense.weight' in k:
                tensorrt_llm_model.layers[
                    idx].attention.dense.weight.value = split(v,
                                                              tensor_parallel,
                                                              rank,
                                                              dim=1)
            elif 'attention.output.dense.bias' in k:
                tensorrt_llm_model.layers[idx].attention.dense.bias.value = v
            elif 'attention.output.LayerNorm.weight' in k:
                tensorrt_llm_model.layers[idx].input_layernorm.weight.value = v
            elif 'attention.output.LayerNorm.bias' in k:
                tensorrt_llm_model.layers[idx].input_layernorm.bias.value = v
            elif 'intermediate.dense.weight' in k:
                tensorrt_llm_model.layers[idx].mlp.fc.weight.value = split(
                    v, tensor_parallel, rank)
            elif 'intermediate.dense.bias' in k:
                tensorrt_llm_model.layers[idx].mlp.fc.bias.value = split(
                    v, tensor_parallel, rank)
            elif 'output.dense.weight' in k:
                tensorrt_llm_model.layers[idx].mlp.proj.weight.value = split(
                    v, tensor_parallel, rank, dim=1)
            elif 'output.dense.bias' in k:
                tensorrt_llm_model.layers[idx].mlp.proj.bias.value = v
            elif 'output.LayerNorm.weight' in k:
                tensorrt_llm_model.layers[idx].post_layernorm.weight.value = v
            elif 'output.LayerNorm.bias' in k:
                tensorrt_llm_model.layers[idx].post_layernorm.bias.value = v
            elif 'attention.self.query.weight' in k:
                qkv_weight[idx][0] = v
            elif 'attention.self.query.bias' in k:
                qkv_bias[idx][0] = v
            elif 'attention.self.key.weight' in k:
                qkv_weight[idx][1] = v
            elif 'attention.self.key.bias' in k:
                qkv_bias[idx][1] = v
            elif 'attention.self.value.weight' in k:
                qkv_weight[idx][2] = v
            elif 'attention.self.value.bias' in k:
                qkv_bias[idx][2] = v

    for i in range(hf_model_config.num_hidden_layers):
        tensorrt_llm_model.layers[i].attention.qkv.weight.value = split(
            np.concatenate(qkv_weight[i]), tensor_parallel, rank)
        tensorrt_llm_model.layers[i].attention.qkv.bias.value = split(
            np.concatenate(qkv_bias[i]), tensor_parallel, rank)


def load_from_hf_qa_model(tensorrt_llm_qa_model: tensorrt_llm.module.Module,
                          hf_qa_model: Union[BertPreTrainedModel,
                                             RobertaPreTrainedModel],
                          hf_bert_config: Union[BertConfig, RobertaConfig],
                          rank=0,
                          tensor_parallel=1,
                          fp16=False):
    load_from_hf_model(tensorrt_llm_qa_model.bert, hf_qa_model, hf_bert_config,
                       rank, tensor_parallel, fp16)
    states = hf_qa_model.state_dict()

    torch_dtype = torch.float16 if fp16 else torch.float32

    tensorrt_llm_qa_model.qa_outputs.weight.value = states[
        'qa_outputs.weight'].to(torch_dtype).cpu().numpy()
    tensorrt_llm_qa_model.qa_outputs.bias.value = states['qa_outputs.bias'].to(
        torch_dtype).cpu().numpy()


def load_from_hf_cls_model(tensorrt_llm_cls_model: tensorrt_llm.models.
                           BertForSequenceClassification,
                           hf_qa_model: Union[BertPreTrainedModel,
                                              RobertaPreTrainedModel],
                           hf_bert_config: Union[BertConfig, RobertaConfig],
                           rank=0,
                           tensor_parallel=1,
                           fp16=False):
    load_from_hf_model(tensorrt_llm_cls_model.bert, hf_qa_model, hf_bert_config,
                       rank, tensor_parallel, fp16)
    states = hf_qa_model.state_dict()

    torch_dtype = torch.float16 if fp16 else torch.float32

    if isinstance(hf_qa_model, BertPreTrainedModel):
        tensorrt_llm_cls_model.pooler.dense.weight.value = states[
            'bert.pooler.dense.weight'].to(torch_dtype).cpu().numpy()
        tensorrt_llm_cls_model.pooler.dense.bias.value = states[
            'bert.pooler.dense.bias'].to(torch_dtype).cpu().numpy()

        tensorrt_llm_cls_model.classifier.weight.value = states[
            'classifier.weight'].to(torch_dtype).cpu().numpy()
        tensorrt_llm_cls_model.classifier.bias.value = states[
            'classifier.bias'].to(torch_dtype).cpu().numpy()
    else:
        tensorrt_llm_cls_model.classifier.dense.weight.value = states[
            'classifier.dense.weight'].to(torch_dtype).cpu().numpy()
        tensorrt_llm_cls_model.classifier.dense.bias.value = states[
            'classifier.dense.bias'].to(torch_dtype).cpu().numpy()

        tensorrt_llm_cls_model.classifier.out_proj.weight.value = states[
            'classifier.out_proj.weight'].to(torch_dtype).cpu().numpy()
        tensorrt_llm_cls_model.classifier.out_proj.bias.value = states[
            'classifier.out_proj.bias'].to(torch_dtype).cpu().numpy()
