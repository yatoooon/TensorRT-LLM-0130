/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2021, NAVER Corp.  Authored by CLOVA.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CUDART_VERSION
#error CUDART_VERSION Undefined!
#elif (CUDART_VERSION >= 11050)
#include <cub/cub.cuh>
#else
#include "3rdparty/cub/cub.cuh"
#endif

#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/common/reduceKernelUtils.cuh"
#include "tensorrt_llm/kernels/samplingTopKKernels.h"

using namespace tensorrt_llm::common;
using namespace tensorrt_llm::runtime;

namespace tensorrt_llm::kernels
{

template <typename T, int32_t BLOCK_SIZE_, int32_t BLOCKS_PER_BEAM_>
__global__ void topKStage1(T const* __restrict logProbs, T const* const* __restrict logProbsPtrs, T* tmpLogProbs,
    SizeType32* topKTmpIdBuf, T* topKTmpValBuf, FinishedState const* finished, SizeType32 maxTopK,
    SizeType32 const* topKs, SizeType32 vocabSize, TokenIdType const* endIds, bool const* skipDecode,
    SizeType32 const* batchSlots, SizeType32 const* tokensPerStep, SizeType32 maxTokensPerStep)
{
    typedef cub::BlockReduce<TopK_2<T>, BLOCK_SIZE_> BlockReduce;
    __shared__ typename BlockReduce::TempStorage tempStorage;

    auto const tid = static_cast<SizeType32>(threadIdx.x);
    auto const bid = static_cast<SizeType32>(blockIdx.x);
    auto const tokenIdx = static_cast<SizeType32>(blockIdx.y);

    auto const batchId = bid / BLOCKS_PER_BEAM_; // row id for logProbs
    auto const batchSlot = batchSlots[batchId];
    if (tokensPerStep != nullptr && tokenIdx >= tokensPerStep[batchSlot])
    {
        return;
    }

    FinishedState const finishState = finished != nullptr ? finished[batchSlot] : FinishedState::empty();
    if ((skipDecode != nullptr && skipDecode[batchSlot]) || (finishState.isSkipDecoding()))
    {
        return;
    }

    auto const logBufIndex = batchId * maxTokensPerStep * vocabSize + tokenIdx * vocabSize;
    auto logProbsSlot
        = logProbsPtrs == nullptr ? logProbs + logBufIndex : logProbsPtrs[batchId * maxTokensPerStep + tokenIdx];

    auto const blockLane = bid % BLOCKS_PER_BEAM_;                  // block id for a beam
    auto const k = (topKs != nullptr) ? topKs[batchSlot] : maxTopK; // batchId = batch index

    auto const tmpLogBufIndex = batchId * maxTokensPerStep * vocabSize + tokenIdx * vocabSize;
    auto const tmpTopKBufIndex = batchId * maxTokensPerStep * BLOCKS_PER_BEAM_ * maxTopK
        + tokenIdx * BLOCKS_PER_BEAM_ * maxTopK + blockLane * k;

    TopK_2<T> partial;
    bool const IS_FP16 = std::is_same<T, half>::value;
    T const MAX_T_VAL = (IS_FP16) ? HALF_FLT_MAX : FLT_MAX;

    if (finished != nullptr && finishState.isFinished())
    {
        if (tid < k)
        {
            auto const index = tmpTopKBufIndex + tid;
            if (blockLane == 0 && tid == 0)
            {
                auto const endId = endIds[batchSlot];
                topKTmpIdBuf[index] = tmpLogBufIndex + endId;
                topKTmpValBuf[index] = logProbsSlot[endId];
            }
            else
            {
                topKTmpIdBuf[index] = -1;
                topKTmpValBuf[index] = -MAX_T_VAL;
            }
        }
        return;
    }

    for (auto elemId = tid + blockLane * BLOCK_SIZE_; elemId < vocabSize; elemId += BLOCK_SIZE_ * BLOCKS_PER_BEAM_)
    {
        auto localIndex = elemId + tmpLogBufIndex;
        tmpLogProbs[localIndex] = logProbsSlot[elemId];
    }

    for (SizeType32 ite = 0; ite < k; ite++)
    {
        partial.init();
#pragma unroll
        for (auto elemId = tid + blockLane * BLOCK_SIZE_; elemId < vocabSize; elemId += BLOCK_SIZE_ * BLOCKS_PER_BEAM_)
        {
            auto index = elemId + tmpLogBufIndex;
            partial.insert(tmpLogProbs[index], index);
        }

        TopK_2<T> total = BlockReduce(tempStorage).Reduce(partial, reduce_topk_op_2<T>);

        if (tid == 0)
        {
            auto const index = tmpTopKBufIndex + ite;
            topKTmpIdBuf[index] = total.p;
            topKTmpValBuf[index] = total.u;
            if (total.p >= 0)
            {
                tmpLogProbs[total.p] = -MAX_T_VAL;
            }
        }
        __syncthreads();
    }
}

template <typename T, int BLOCK_SIZE_, int BLOCKS_PER_BEAM_>
__global__ void topKStage2Sampling(SizeType32 const* __restrict topKTmpIdBuf, T* topKTmpValBuf, TokenIdType** idsPtrs,
    TokenIdType* ids, SizeType32* sequenceLengths, FinishedState const* finishedInput, FinishedState* finishedOutput,
    float* cumLogProbs, float* outputLogProbs, SizeType32 maxTopK, SizeType32 const* topKs, float topP,
    float const* topPs, curandState_t* curandState, TokenIdType const* endIds, SizeType32 vocabSize,
    bool const* skipDecode, SizeType32 const* batchSlots, SizeType32 maxBatchSize, bool normalizeLogProbs,
    bool logitHasProbs, SizeType32 const* tokensPerStep, SizeType32 maxTokensPerStep, SizeType32 maxSeqLen,
    bool returnAllTopK)
{
    bool const IS_FP16 = std::is_same<T, half>::value;
    T const MAX_T_VAL = (IS_FP16) ? HALF_FLT_MAX : FLT_MAX;

    auto const tid = static_cast<SizeType32>(threadIdx.x);
    auto const batchIdx = static_cast<SizeType32>(blockIdx.x);
    auto const tokenIdx = static_cast<SizeType32>(blockIdx.y);
    auto const batchSlot = batchSlots[batchIdx];
    FinishedState const finishState = finishedInput != nullptr ? finishedInput[batchSlot] : FinishedState::empty();
    if ((skipDecode != nullptr && skipDecode[batchSlot]) || (finishState.isSkipDecoding()))
    {
        return;
    }
    if (tokensPerStep != nullptr && tokenIdx >= tokensPerStep[batchSlot])
    {
        return;
    }

    auto const k = (topKs != nullptr) ? topKs[batchSlot] : maxTopK;
    auto const probThreshold = (topPs != nullptr) ? topPs[batchSlot] : topP;
    auto const size = k * BLOCKS_PER_BEAM_;
    auto const stride = maxTopK * BLOCKS_PER_BEAM_;

    typedef cub::BlockReduce<TopK_2<float>, BLOCK_SIZE_> BlockReduce;
    __shared__ typename BlockReduce::TempStorage tempStorage;
    extern __shared__ char array[];
    __shared__ float sSum;
    T* sVal = topKTmpValBuf + (batchIdx * maxTokensPerStep + tokenIdx) * stride;
    auto* sId = reinterpret_cast<SizeType32*>(array);
    if (tid == 0)
    {
        sSum = 0.0f;
    }
    TopK_2<float> partial;

    if (finishState.isFinished())
    {
        if (finishedOutput != nullptr)
        {
            finishedOutput[batchSlot] = finishState;
        }
        return;
    }

    auto sVal2 = reinterpret_cast<float*>(sId + k);
    float maxLogit;
    for (SizeType32 ite = 0; ite < k; ite++)
    {
        partial.init();
#pragma unroll
        for (SizeType32 i = tid; i < size; i += BLOCK_SIZE_)
        {
            partial.insert((float) sVal[i], i);
        }

        TopK_2<float> total = BlockReduce(tempStorage).Reduce(partial, reduce_topk_op_2<float>);

        if (tid == 0)
        {
            if (ite == 0)
            {
                maxLogit = total.u;
            }
            sId[ite] = total.p;
            sVal[total.p] = -MAX_T_VAL;

            // when cumLogProbs are computed, topKTmpValBuf (logits_buf_) are
            // already pre-processed by softmax_kernel
            if (!logitHasProbs)
            {
                total.u = __expf(total.u - maxLogit);
            }
            sVal2[ite] = total.u;
            sSum += total.u;
        }
        __syncthreads();
    }

    if (tid == 0)
    {
        auto randNum = static_cast<float>(curand_uniform(curandState + batchSlot) * probThreshold * sSum);
        auto* outputIdsRequestPtr = idsPtrs == nullptr ? ids + batchSlot * maxSeqLen : idsPtrs[batchSlot];
        for (SizeType32 ki = 0; ki < k; ki++)
        {
            auto expLogit = sVal2[ki];
            randNum = randNum - expLogit;
            if (randNum <= 0.0f || ki == k - 1 || returnAllTopK)
            {
                auto idx = sId[ki];
                // If sId is -1 here we force output token to the last from vocabulary to get vivid indicator of smth
                // going wrong for the debug
                auto outputId = idx != -1
                    ? topKTmpIdBuf[(batchIdx * maxTokensPerStep + tokenIdx) * stride + idx] % vocabSize
                    : vocabSize - 1;
                auto const curSeqLen = sequenceLengths == nullptr ? 0 : sequenceLengths[batchSlot];
                auto const outIdx = returnAllTopK ? tokenIdx * maxTopK + ki : curSeqLen + tokenIdx;
                outputIdsRequestPtr[outIdx] = outputId;
                // cum log prob is not supported with returnAllTopK
                if (!returnAllTopK)
                {
                    if (cumLogProbs != nullptr || outputLogProbs != nullptr)
                    {
                        auto logProb = logf(expLogit);
                        if (cumLogProbs != nullptr)
                        {
                            cumLogProbs[batchSlot] += logProb;
                        }
                        if (outputLogProbs != nullptr)
                        {
                            // 'outputLogProbs' is the probability induced by the top-k sampling:
                            // NOT normalized (same way as OpenAI does):
                            // log_prob = log P(i | i is in vocab) = log(expLogit)
                            // normalized:
                            // log_prob = log P(i | i is in top-k) = log(expLogit / sum)
                            outputLogProbs[curSeqLen * maxBatchSize + batchSlot]
                                = normalizeLogProbs ? logProb - logf(sSum) : logProb;
                        }
                    }
                    break;
                }
            }
        }
        if (maxTokensPerStep == 1 && !returnAllTopK && sequenceLengths != nullptr && finishedOutput != nullptr
            && endIds != nullptr)
        {
            auto const seqLen = sequenceLengths[batchSlot];
            if (outputIdsRequestPtr[seqLen] == endIds[batchSlot])
            {
                finishedOutput[batchSlot].setFinishedEOS();
                // Do not increase seq len when EOS is generated. Seq len should always contain only tokens to be
                // outputted
            }
            else
            {
                // We don't need to set output finished state as it is assumed to be in non finished state
                sequenceLengths[batchSlot] += 1;
            }
        }
    }
}

#define CASE_K(K_MAX, BLOCK_SIZE_1_, BLOCK_SIZE_2_, BLOCKS_PER_BEAM_)                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        {                                                                                                              \
            dim3 grid(params.batchSize* BLOCKS_PER_BEAM_, params.maxTokensPerStep);                                    \
            dim3 block(BLOCK_SIZE_1_);                                                                                 \
            topKStage1<T, BLOCK_SIZE_1_, BLOCKS_PER_BEAM_><<<grid, block, 0, stream>>>(params.logProbs,                \
                params.logProbsPtrs, tempLogProbs, topKTmpIdBuf, topKTmpValBuf, params.finishedInput, params.maxTopK,  \
                params.topKs, params.vocabSizePadded, params.endIds, params.skipDecode, params.batchSlots,             \
                params.tokensPerStep, params.maxTokensPerStep);                                                        \
        }                                                                                                              \
        {                                                                                                              \
            dim3 grid(params.batchSize, params.maxTokensPerStep);                                                      \
            dim3 block(BLOCK_SIZE_2_);                                                                                 \
            topKStage2Sampling<T, BLOCK_SIZE_2_, BLOCKS_PER_BEAM_>                                                     \
                <<<grid, block, K_MAX * sizeof(SizeType32) + K_MAX * sizeof(float), stream>>>(topKTmpIdBuf,            \
                    topKTmpValBuf, params.outputIdsPtrs, params.outputIds, params.sequenceLengths,                     \
                    params.finishedInput, params.finishedOutput, params.cumLogProbs, params.outputLogProbs,            \
                    params.maxTopK, params.topKs, params.maxTopP, params.topPs, params.curandState, params.endIds,     \
                    params.vocabSizePadded, params.skipDecode, params.batchSlots, params.maxBatchSize,                 \
                    params.normalizeLogProbs, params.logitsHasProbs, params.tokensPerStep, params.maxTokensPerStep,    \
                    params.maxSeqLen, params.returnAllTopK);                                                           \
        }                                                                                                              \
    } while (0)

template <typename T>
void invokeBatchTopKSampling(TopKSamplingKernelParams<T> const& params, cudaStream_t stream)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

    params.checkParams();

    // Not allow an ambiguous inputs topP and topPs.
    auto const workspaceSizes
        = getTopKWorkspaceSizes<T>(params.batchSize, params.maxTokensPerStep, params.maxTopK, params.vocabSizePadded);

    if (params.maxTopK == 0)
    {
        return;
    }

    std::vector<void*> alignedPointers;
    calcAlignedPointers(alignedPointers, params.workspace, workspaceSizes);

    auto tempLogProbs = static_cast<T*>(alignedPointers[0]);
    auto topKTmpIdBuf = static_cast<SizeType32*>(alignedPointers[1]);
    auto topKTmpValBuf = static_cast<T*>(alignedPointers[2]);

    SizeType32 logMaxTopK{0};
    SizeType32 recursor{params.maxTopK - 1};
    while (recursor >>= 1)
    {
        ++logMaxTopK;
    }

    switch (logMaxTopK)
    {
    case 0:
    case 1:
    case 2:
    case 3: // 0 < maxTopK <= 16
        CASE_K(16, 128, 128, 8);
        break;
    case 4: // 16 < maxTopK <= 32
        CASE_K(32, 256, 128, 8);
        break;
    case 5: // 32 < maxTopK <= 64
        CASE_K(64, 256, 256, 8);
        break;
    case 6:
    case 7:
    case 8:
    case 9: // 64 < maxTopK <= 1024
        CASE_K(1024, 256, 256, 8);
        break;
    default: TLLM_CHECK_WITH_INFO(false, "TopK kernel supports 1 <= k <= 1024 but got k=%d", params.maxTopK);
    }

    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

#undef CASE_K

template void invokeBatchTopKSampling(TopKSamplingKernelParams<float> const& params, cudaStream_t stream);

template void invokeBatchTopKSampling(TopKSamplingKernelParams<half> const& params, cudaStream_t stream);

__global__ void setupTopKRuntimeArgs(SizeType32 batchSize, SizeType32 topK, SizeType32* topKs, SizeType32 topKsSize,
    float topP, float* topPs, SizeType32 topPsSize, bool* skipDecode, SizeType32 const* batchSlots)
{
    auto const index = static_cast<SizeType32>(blockIdx.x * blockDim.x + threadIdx.x);
    for (auto bi = index; bi < batchSize; bi += static_cast<SizeType32>(gridDim.x * blockDim.x))
    {
        auto const batchSlot = batchSlots[bi];
        auto k = topKsSize > 1 ? topKs[batchSlot] : topK;
        auto p = topPsSize > 1 ? topPs[batchSlot] : topP;

        if (k == 0 && p == 0.0f)
        {
            // TensorRT-LLM's topp implementation does not support topp = 0.0f, but it
            // equivalent to greedy search. So, we set the topk = 1 as an alternative
            // solution.
            k = 1;
        }
        if (k > 0 && p == 0.0f)
        {
            // This case corresponds to the old topk sampling, which is equivalent to
            // the old topk_topp sampling with topp=1.0f. TopKSamplingLayer and
            // TopKTopPSamplingLayer are now merged by TopKSamplingLayer. Thus, we
            // replace the case topk>0 and topp=0.0f by topk>0 and topp=1.0f for the
            // compatibility.
            p = 1.0f;
        }
        topKs[batchSlot] = k;
        topPs[batchSlot] = p;
        skipDecode[batchSlot] = k == 0;
    }
}

void invokeSetupTopKRuntimeArgs(SizeType32 batchSize, SizeType32 topK, SizeType32* runtimeTopKDevicePtr,
    SizeType32 runtimeTopKSize, float topP, float* runtimeTopPDevicePtr, SizeType32 runtimeTopPSize,
    bool* skipDecodeDevicePtr, SizeType32 const* batchSlotsDevicePtr, cudaStream_t stream)
{
    dim3 block(std::min(static_cast<uint32_t>(batchSize), 256u));
    dim3 grid(divUp(static_cast<uint32_t>(batchSize), block.x));
    // support topK up to TOP_K_MAX.
    setupTopKRuntimeArgs<<<grid, block, 0, stream>>>(batchSize, topK, runtimeTopKDevicePtr, runtimeTopKSize, topP,
        runtimeTopPDevicePtr, runtimeTopPSize, skipDecodeDevicePtr, batchSlotsDevicePtr);
}

} // namespace tensorrt_llm::kernels
