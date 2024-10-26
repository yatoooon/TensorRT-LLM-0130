from __future__ import annotations

import json
from collections import defaultdict, namedtuple
from pathlib import Path
from typing import Dict, List, Tuple, Union

import tensorrt_llm.bindings.executor as trtllm
from tensorrt_llm.bench.run.dataclasses import (BenchmarkStatistics,
                                                PercentileStats, RequestRecord)
from tensorrt_llm.bindings import InferenceRequest

ResponseTuple = namedtuple(
    "ResponseTuple", ["timestamp", "request_id", "final", "error", "tokens"])


def get_executor_request(request: InferenceRequest,
                         pad_id: int,
                         eos_id: int,
                         streaming: bool = False) -> trtllm.Request:
    return trtllm.Request(
        input_token_ids=request.logits,
        max_tokens=request.output_tokens,
        stop_words=[],
        bad_words=[],
        streaming=streaming,
        output_config=trtllm.OutputConfig(exclude_input_from_output=True),
        pad_id=pad_id,
        end_id=eos_id,
    )


def get_settings_from_engine(
    engine_path: Path
) -> Tuple[Dict[str, Union[str, int]], Dict[str, Union[str, int]]]:
    config_path = engine_path / "config.json"
    runtime_config = {}

    with open(config_path, "r") as config_json:
        config = json.load(config_json)

    engine_world_map = config["pretrained_config"]["mapping"]
    engine_build_cfg = config["build_config"]
    engine_parallel_map = engine_build_cfg["auto_parallel_config"]

    world_config = {
        "pp_size": engine_world_map["pp_size"],
        "tp_size": engine_world_map["tp_size"],
        "world_size": engine_world_map["world_size"],
        "gpus_per_node": engine_parallel_map["gpus_per_node"],
    }

    executor_settings = {
        "max_batch_size": engine_build_cfg["max_batch_size"],
        "max_num_tokens": engine_build_cfg["max_num_tokens"],
    }

    runtime_config.update({
        "sw_version": config["version"],
        "engine_dir": str(engine_path.absolute()),
        "settings_config": executor_settings,
        "world_config": world_config,
    })

    return runtime_config, engine_build_cfg


class StatsKeeper:

    def __init__(self) -> None:
        self.requests: Dict[RequestRecord] = defaultdict(RequestRecord)
        self.num_complete: int = 0

    def register_request(
        self,
        request_id: int,
        timestamp: float,
        num_tokens: int,
    ) -> None:
        record = self.requests[request_id]
        record.num_input_tokens = num_tokens
        record.start_timestamp = timestamp

    def register_response(self, request_id: int, timestamp: int, final: bool,
                          error: bool, tokens: List[int]) -> None:
        record = self.requests[request_id]
        record.register_event(error, final, timestamp, tokens)
        if final:
            self.num_complete = self.num_complete + 1

    def generate_statistics_summary(self) -> None:
        total_output_tokens: int = 0
        total_input_tokens: int = 0
        num_requests = len(self.requests)
        start_time = float("inf")
        end_time = -1

        request_latencies = []
        intertoken_avg_latencies = []
        ttft_times = []
        last_queue_time = 0.0
        queue_time_total = 0.0

        for entry in self.requests.values():
            start_time = min(entry.start_timestamp, start_time)
            end_time = max(entry.end_timestamp, end_time)
            queue_time_total += entry.start_timestamp - last_queue_time
            last_queue_time = entry.start_timestamp

            request_latencies.append(entry.end_to_end_latency)
            ttft_times.append(entry.time_to_first_token)
            intertoken_avg_latencies.append(entry.intertoken_latency)

            total_output_tokens += entry.num_output_tokens
            total_input_tokens += entry.num_input_tokens

        stats = BenchmarkStatistics(
            num_requests=num_requests,
            total_latency_ns=end_time - start_time,
            total_output_tokens=total_output_tokens,
            total_input_tokens=total_input_tokens,
            request_percentiles=PercentileStats.from_iterable(
                request_latencies),
            itl_percentiles=PercentileStats.from_iterable(
                intertoken_avg_latencies),
            ttft_percentiles=PercentileStats.from_iterable(ttft_times),
            issue_rate_ns=queue_time_total / num_requests)

        return stats
