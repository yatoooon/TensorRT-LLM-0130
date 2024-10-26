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
import os

# isort: off
import torch
import tensorrt as trt
#isort: on
from base_benchmark import BaseBenchmark

import tensorrt_llm
from tensorrt_llm._utils import trt_dtype_to_torch
from tensorrt_llm.runtime import TensorInfo


class BERTBenchmark(BaseBenchmark):

    def __init__(self, args, batch_sizes, in_lens, gpu_weights_percents, rank,
                 world_size):
        super().__init__(args.engine_dir, args.model, args.dtype, rank,
                         world_size)
        self.batch_sizes = batch_sizes
        self.in_lens = in_lens
        self.build_time = 0
        self.gpu_weights_percents = gpu_weights_percents

        # Deserialize engine from engine directory
        self.serialize_path = os.path.join(args.engine_dir, self.engine_name)
        with open(self.serialize_path, 'rb') as f:
            engine_buffer = f.read()
            assert engine_buffer is not None

        self.session = tensorrt_llm.runtime.Session.from_serialized_engine(
            engine_buffer)

        # Print context memory size for CI/CD to track.
        context_mem_size = self.session.context_mem_size
        print(
            f"Allocated {context_mem_size / 1048576.0:.2f} MiB for execution context memory."
        )

    def get_config(self):
        for inlen in self.in_lens:
            if inlen > self.max_input_len:
                continue
            for batch_size in self.batch_sizes:
                if batch_size > self.max_batch_size:
                    continue
                for gpu_weights_percent in self.gpu_weights_percents:
                    yield (batch_size, inlen, gpu_weights_percent)

    def set_weight_streaming(self, config):
        gpu_weights_percent = config[2]
        self.session._set_weight_streaming(gpu_weights_percent)

    def prepare_inputs(self, config):
        batch_size, inlen = config[0], config[1]
        input_ids = torch.randint(100, (batch_size, inlen)).int().cuda()
        input_lengths = inlen * torch.ones(
            (batch_size, ), dtype=torch.int32, device='cuda')
        inputs = {'input_ids': input_ids, 'input_lengths': input_lengths}
        output_info = self.session.infer_shapes([
            TensorInfo('input_ids', trt.DataType.INT32, input_ids.shape),
            TensorInfo('input_lengths', trt.DataType.INT32, input_lengths.shape)
        ])
        outputs = {
            t.name: torch.empty(tuple(t.shape),
                                dtype=trt_dtype_to_torch(t.dtype),
                                device='cuda')
            for t in output_info
        }
        stream = torch.cuda.current_stream().cuda_stream
        return (inputs, outputs, stream)

    def run(self, inputs, config, benchmark_profiler=None):
        ok = self.session.run(*inputs)
        assert ok, "Runtime execution failed"
        torch.cuda.synchronize()

    def report(self, config, latency, percentile95, percentile99,
               peak_gpu_used):
        if self.runtime_rank == 0:
            line = '[BENCHMARK] ' + (
                f'model_name {self.model_name} world_size {self.world_size} precision {self.dtype} '
                f'batch_size {config[0]} input_length {config[1]} gpu_peak_mem(gb) {peak_gpu_used} '
                f'build_time(s) {self.build_time} percentile95(ms) {percentile95} '
                f'percentile99(ms) {percentile99} latency(ms) {latency}')
            print(line)

    def report(self,
               config,
               latency,
               percentile95,
               percentile99,
               peak_gpu_used,
               csv,
               benchmark_profiler=None):
        report_dict = super().get_report_dict()
        batch_size, inlen = config[0], config[1]
        report_dict["num_heads"] = self.num_heads
        report_dict["num_kv_heads"] = self.num_heads
        report_dict["num_layers"] = self.num_layers
        report_dict["hidden_size"] = self.hidden_size
        report_dict["vocab_size"] = self.vocab_size
        report_dict["batch_size"] = batch_size
        report_dict["input_length"] = inlen
        report_dict["output_length"] = "n/a"
        report_dict["gpu_weights_percent"] = config[2]
        report_dict["latency(ms)"] = latency
        report_dict["build_time(s)"] = self.build_time
        report_dict["tokens_per_sec"] = "n/a"
        report_dict["percentile95(ms)"] = percentile95
        report_dict["percentile99(ms)"] = percentile99
        report_dict["gpu_peak_mem(gb)"] = peak_gpu_used
        if self.runtime_rank == 0:
            if csv:
                line = ",".join([str(v) for v in report_dict.values()])
                print(line)
                with open(self.get_csv_filename(), "a") as file:
                    file.write(line + "\n")
            else:
                kv_pairs = [f"{k} {v}" for k, v in report_dict.items()]
                line = '[BENCHMARK] ' + " ".join(kv_pairs)
                print(line)
