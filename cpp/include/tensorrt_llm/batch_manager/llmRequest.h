/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION.  All rights reserved.
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

#pragma once

#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/executor/executor.h"
#include "tensorrt_llm/runtime/bufferManager.h"
#include "tensorrt_llm/runtime/decodingOutput.h"
#include "tensorrt_llm/runtime/iBuffer.h"
#include "tensorrt_llm/runtime/iTensor.h"
#include "tensorrt_llm/runtime/samplingConfig.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tensorrt_llm::batch_manager
{

/**
 * @brief The state of the request.
 *
 * Enum order must follow chronological order for state dependency check, @see hasReachedState().
 *
 * @todo(rkobus): refactor
 */
enum LlmRequestState_t
{
    REQUEST_STATE_UNKNOWN = 0,                          ///< Unknown state
    REQUEST_STATE_ENCODER_INIT = 1,                     ///< Encoder phase starts (for encoder-decoder models)
    REQUEST_STATE_CONTEXT_INIT = 2,                     ///< Context phase starts
    REQUEST_STATE_GENERATION_IN_PROGRESS = 3,           ///< Generation phase is in progress
    REQUEST_STATE_GENERATION_TO_COMPLETE = 4,           ///< Generation phase is to be completed
    REQUEST_STATE_GENERATION_COMPLETE = 5,              ///< Generation phase completed
    REQUEST_STATE_DISAGG_GENERATION_INIT = 6,           ///< For disaggregated serving only:
                                                        /// new Generation request arrived at generation model
    REQUEST_STATE_DISAGG_CONTEXT_TRANS_IN_PROGRESS = 7, ///< For disaggregated serving only:
                                                        /// Waiting context-only request transmitting the kv cache
    REQUEST_STATE_DISAGG_CONTEXT_COMPLETE = 8,          ///< Context-only request finished kv cache transmission.
    REQUEST_STATE_DISAGG_GENERATION_TRANS_IN_PROGRESS
    = 9,                                                ///< For disaggregated serving only: transmitting the kv cache
};

enum LlmRequestType
{
    LLMREQUEST_TYPE_CONTEXT_AND_GENERATION = 0, // Normal request will inference both context phase and generation phase
    LLMREQUEST_TYPE_CONTEXT_ONLY = 1,           // Only inference context phase
    LLMREQUEST_TYPE_GENERATION_ONLY = 2         // only inference generation phase
};

template <typename TTensor, typename TStream = runtime::BufferManager::CudaStreamPtr>
class GenericLlmRequest
{
public:
    using SizeType32 = runtime::SizeType32;
    using TokenIdType = runtime::TokenIdType;
    using RequestIdType = std::uint64_t;
    using LoraTaskIdType = runtime::LoraTaskIdType;
    using VecTokens = std::vector<TokenIdType>;
    using TokenExtraIdType = runtime::TokenExtraIdType;
    using VecTokenExtraIds = runtime::VecTokenExtraIds;
    using VecLogProbs = std::vector<float>;
    using BeamTokens = std::vector<VecTokens>;
    using UniqueToken = runtime::UniqueToken;
    using VecUniqueTokens = runtime::VecUniqueTokens;
    using BeamUniqueTokens = std::vector<VecUniqueTokens>;
    using TensorPtr = TTensor;
    using LogitsPostProcessor = std::function<void(
        RequestIdType, TensorPtr&, BeamTokens const&, TStream const&, std::optional<RequestIdType>)>;
    using RequestPtr = std::shared_ptr<GenericLlmRequest>;

    GenericLlmRequest(RequestIdType requestId, SizeType32 maxNewTokens, std::shared_ptr<VecTokens> inputTokens,
        runtime::SamplingConfig const& samplingConfig, bool isStreaming, std::optional<SizeType32> endId = std::nullopt,
        std::optional<SizeType32> padId = std::nullopt, std::optional<TensorPtr> embeddingBias = std::nullopt,
        std::optional<TensorPtr> badWordsList = std::nullopt, std::optional<TensorPtr> stopWordsList = std::nullopt,
        std::optional<std::shared_ptr<std::vector<SizeType32>>> positionIds = std::nullopt,
        std::optional<TensorPtr> promptEmbeddingTable = std::nullopt,
        std::optional<SizeType32> promptVocabSize = std::nullopt,
        std::optional<LoraTaskIdType> loraTaskId = std::nullopt, std::optional<TensorPtr> loraWeights = std::nullopt,
        std::optional<TensorPtr> loraConfig = std::nullopt,
        std::optional<executor::LookaheadDecodingConfig> lookaheadConfig = std::nullopt, bool returnLogProbs = false,
        bool returnContextLogits = false, bool returnGenerationLogits = false,
        std::optional<std::shared_ptr<VecTokens>> draftTokens = std::nullopt,
        std::optional<TensorPtr> draftLogits = std::nullopt, bool excludeInputFromOutput = false,
        std::optional<LogitsPostProcessor> logitsPostProcessor = std::nullopt,
        bool applyLogitsPostProcessorBatched = false,
        std::optional<std::shared_ptr<VecTokens>> encoderInputTokens = std::nullopt, bool returnEncoderOutput = false,
        std::optional<RequestIdType> clientId = std::nullopt,
        executor::PriorityType priority = executor::Request::kDefaultPriority,
        std::optional<TensorPtr> encoderInputFeatures = std::nullopt,
        std::optional<SizeType32> encoderOutputLength = std::nullopt,
        LlmRequestType llmRequestType = LlmRequestType::LLMREQUEST_TYPE_CONTEXT_AND_GENERATION,
        std::optional<std::shared_ptr<VecTokenExtraIds>> inputTokenExtraIds = std::nullopt,
        SizeType32 numReturnSequences = 1)
        : mRequestId(requestId)
        , mPromptLen(inputTokens->size())
        , mMaxNewTokens(maxNewTokens)
        , mSamplingConfig(samplingConfig)
        , mState(REQUEST_STATE_CONTEXT_INIT)
        , mEndId(endId)
        , mPadId(padId)
        , mLogitsPostProcessor(logitsPostProcessor)
        , mApplyLogitsPostProcessorBatched(applyLogitsPostProcessorBatched)
        , mClientId(clientId)
        , mIsStreaming(isStreaming)
        , mOrigPromptLen(mPromptLen)
        , mNumPreDecodedTokens(samplingConfig.beamWidth, 0)
        , mMaxSentTokenLen(mPromptLen)
        , mEmbeddingBias(std::move(embeddingBias))
        , mBadWordsList(std::move(badWordsList))
        , mStopWordsList(std::move(stopWordsList))
        , mPositionIds(std::move(positionIds))
        , mPromptEmbeddingTable(std::move(promptEmbeddingTable))
        , mPromptVocabSize(promptVocabSize)
        , mLoraTaskId(loraTaskId)
        , mLoraWeights(std::move(loraWeights))
        , mLoraConfig(std::move(loraConfig))
        , mLookaheadConfig(std::move(lookaheadConfig))
        , mContextChunkSize(std::nullopt)
        , mContextCurrentPosition(0)
        , mLogProbs(samplingConfig.beamWidth)
        , mCumLogProbs(samplingConfig.beamWidth)
        , mDraftTokens(draftTokens.value_or(std::make_shared<VecTokens>()))
        , mDraftLogits(draftLogits)
        , mNumTokensPerIteration(1)
        , mReturnAllGeneratedTokens(isStreaming && (samplingConfig.beamWidth > 1))
        , mReturnContextLogits(returnContextLogits)
        , mReturnGenerationLogits(returnGenerationLogits)
        , mExcludeInputFromOutput(excludeInputFromOutput)
        , mEncoderTokens(std::move(encoderInputTokens))
        , mReturnEncoderOutput(returnEncoderOutput)
        , mDecodingIter(0)
        , mPriority(priority)
        , mFinishReasons(samplingConfig.beamWidth)
        , mEncoderInputFeatures(std::move(encoderInputFeatures))
        , mEncoderOutputLength(encoderOutputLength)
        , mLlmRequestType(llmRequestType)
        , mInputTokenExtraIds(std::move(inputTokenExtraIds))
        , mNumReturnSequences(numReturnSequences)
        , mSequenceIndex(0)
    {
        if (mEncoderTokens.has_value() || encoderInputFeatures.has_value())
        {
            mState = REQUEST_STATE_ENCODER_INIT;
        }

        initialize(*inputTokens, returnLogProbs);
    }

    GenericLlmRequest(RequestIdType requestId, executor::Request const& req)
        : mRequestId(requestId)
        , mPromptLen(req.getInputTokenIds().size())
        , mMaxNewTokens(req.getMaxTokens())
        , mSamplingConfig(req.getSamplingConfig(), req.getExternalDraftTokensConfig())
        , mState(REQUEST_STATE_CONTEXT_INIT)
        , mEndId(req.getEndId())
        , mPadId(req.getPadId())
        , mClientId(req.getClientId())
        , mIsStreaming(req.getStreaming())
        , mOrigPromptLen(mPromptLen)
        , mNumPreDecodedTokens(mSamplingConfig.beamWidth, 0)
        , mMaxSentTokenLen(mPromptLen)
        , mEmbeddingBias(std::nullopt)
        , mBadWordsList(std::nullopt)
        , mStopWordsList(std::nullopt)
        , mPositionIds(std::nullopt)
        , mPromptEmbeddingTable(std::nullopt)
        , mPromptVocabSize(std::nullopt)
        , mLoraTaskId(std::nullopt)
        , mLoraWeights(std::nullopt)
        , mLoraConfig(std::nullopt)
        , mLookaheadConfig(std::nullopt)
        , mContextChunkSize(std::nullopt)
        , mContextCurrentPosition(0)
        , mLogProbs(mSamplingConfig.beamWidth)
        , mCumLogProbs(mSamplingConfig.beamWidth)
        , mDraftTokens(std::make_shared<VecTokens>())
        , mDraftLogits(std::nullopt)
        , mNumTokensPerIteration(1)
        , mReturnAllGeneratedTokens(req.getReturnAllGeneratedTokens())
        , mReturnContextLogits(req.getOutputConfig().returnContextLogits)
        , mReturnGenerationLogits(req.getOutputConfig().returnGenerationLogits)
        , mExcludeInputFromOutput(req.getOutputConfig().excludeInputFromOutput)
        , mEncoderTokens(std::nullopt)
        , mReturnEncoderOutput(req.getOutputConfig().returnEncoderOutput)
        , mDecodingIter(0)
        , mPriority(req.getPriority())
        , mFinishReasons(mSamplingConfig.beamWidth)
        , mEncoderOutputLength(req.getEncoderOutputLength())
        , mContextPhaseParams(req.getContextPhaseParams())
        , mInputTokenExtraIds(std::nullopt)
        , mNumReturnSequences(req.getNumReturnSequences())
        , mSequenceIndex(0)
    {
        if (req.getRequestType() == executor::RequestType::REQUEST_TYPE_GENERATION_ONLY)
        {
            mState = REQUEST_STATE_DISAGG_GENERATION_INIT;
        }
        if (mIsStreaming && mSamplingConfig.beamWidth > 1 && !mReturnAllGeneratedTokens)
        {
            TLLM_LOG_WARNING(
                "Setting mReturnAllGeneratedTokens to True since streaming AND beam search are done simultaneously. "
                "Returning the full beams at each streaming step is needed because beam search + streaming can change "
                "previous outputs. Initialize request with mReturnAllGeneratedTokens = True to dismiss this error. "
                "WARNING: using this option may increase network usage significantly (quadratically w.r.t output "
                "length).");
            mReturnAllGeneratedTokens = true;
        }

        if (mIsStreaming && mSamplingConfig.beamWidth > 1 && mReturnGenerationLogits == true)
        {
            TLLM_LOG_WARNING(
                "Returning generation logits when streaming is enabled and beamWidth > 1 is not allowed. "
                "This is because the logits may appear in irrelevant order when the beams are gathered, "
                "since logits are not. Disabling returnGenerationLogits.");
            mReturnGenerationLogits = false;
        }

        if (req.getEncoderInputTokenIds().has_value() || req.getEncoderInputFeatures().has_value())
        {
            mState = REQUEST_STATE_ENCODER_INIT;
            if (req.getEncoderInputTokenIds().has_value())
            {
                mEncoderTokens = std::make_shared<VecTokens>(req.getEncoderInputTokenIds().value());
            }
        }

        if (req.getEmbeddingBias())
        {
            mEmbeddingBias = executor::detail::toITensor(req.getEmbeddingBias().value());
            // Add leading 1 dimension since that's what IFB code expects
            mEmbeddingBias.value()->unsqueeze(0);
        }
        if (req.getBadWords())
        {
            mBadWordsList = createListTensor(req.getBadWords().value());
        }
        if (req.getStopWords())
        {
            mStopWordsList = createListTensor(req.getStopWords().value());
        }

        if (req.getPositionIds())
        {
            mPositionIds = std::make_shared<std::vector<SizeType32>>(req.getPositionIds().value());
        }

        auto pTuningConfig = req.getPromptTuningConfig();
        if (pTuningConfig)
        {
            mPromptEmbeddingTable = executor::detail::toITensor(pTuningConfig.value().getEmbeddingTable());
            TLLM_CHECK(mPromptEmbeddingTable.value()->getShape().nbDims == 2);
            mPromptVocabSize = mPromptEmbeddingTable.value()->getShape().d[0];
            mPromptEmbeddingTable.value()->unsqueeze(0);

            if (pTuningConfig->getInputTokenExtraIds())
            {
                mInputTokenExtraIds
                    = std::make_shared<VecTokenExtraIds>(pTuningConfig->getInputTokenExtraIds().value());
            }
        }

        auto loraConfig = req.getLoraConfig();
        if (loraConfig)
        {
            mLoraTaskId = loraConfig->getTaskId();
            if (loraConfig.value().getWeights())
            {
                mLoraWeights = tensorrt_llm::runtime::ITensor::view(
                    executor::detail::toITensor(loraConfig.value().getWeights().value()));
                mLoraWeights.value()->unsqueeze(0);
            }

            if (loraConfig.value().getConfig())
            {
                mLoraConfig = tensorrt_llm::runtime::ITensor::view(
                    executor::detail::toITensor(loraConfig.value().getConfig().value()));
                mLoraConfig.value()->unsqueeze(0);
            }
        }

        auto lookaheadConfig = req.getLookaheadConfig();
        if (lookaheadConfig)
        {
        }

        auto externalDraftTokensConfig = req.getExternalDraftTokensConfig();
        if (externalDraftTokensConfig)
        {
            mDraftTokens = std::make_shared<VecTokens>(externalDraftTokensConfig.value().getTokens());

            if (externalDraftTokensConfig.value().getLogits())
            {
                mDraftLogits = executor::detail::toITensor(externalDraftTokensConfig.value().getLogits().value());
            }

            // NOTE: Draft acceptance threshold is stored in mSamplingConfig
        }

        auto const& encoderInputFeatures = req.getEncoderInputFeatures();
        if (encoderInputFeatures.has_value())
        {
            mEncoderInputFeatures = executor::detail::toITensor(encoderInputFeatures.value());
        }
        else
        {
            mEncoderInputFeatures = std::nullopt;
        }

        switch (req.getRequestType())
        {
        case executor::RequestType::REQUEST_TYPE_CONTEXT_AND_GENERATION:
            mLlmRequestType = LlmRequestType::LLMREQUEST_TYPE_CONTEXT_AND_GENERATION;
            break;
        case executor::RequestType::REQUEST_TYPE_CONTEXT_ONLY:
            mLlmRequestType = LlmRequestType::LLMREQUEST_TYPE_CONTEXT_ONLY;
            break;
        case executor::RequestType::REQUEST_TYPE_GENERATION_ONLY:
            mLlmRequestType = LlmRequestType::LLMREQUEST_TYPE_GENERATION_ONLY;
            break;
        default: throw std::runtime_error("Unsupported request type found.");
        }

        initialize(req.getInputTokenIds(), req.getOutputConfig().returnLogProbs);
    }

    void validate(SizeType32 maxInputLen, SizeType32 maxSequenceLen, SizeType32 maxDraftLen,
        std::optional<SizeType32> maxEncoderInputLen = std::nullopt, bool enableKVCacheReuse = false)
    {
        TLLM_CHECK_WITH_INFO(!(maxEncoderInputLen.has_value() && getEncoderInputLen() > maxEncoderInputLen.value()),
            "Encoder length (%d) exceeds maximum encoder input length (%d).", getEncoderInputLen(),
            maxEncoderInputLen.value());

        if (mPromptLen > maxInputLen)
        {
            TLLM_THROW(
                "Prompt length (%d) exceeds maximum input length (%d). Set log level to info and check "
                "TRTGptModel logs for how maximum input length is set",
                mPromptLen, maxInputLen);
        }

        // Maximum number of draft tokens per request we pass to the engine for single runtime iteration.
        // It depends on the speculative decoding mode.
        auto draftLenPerEngineStep = maxDraftLen;
        auto const& draftTokens = getDraftTokens();
        if (draftTokens && !draftTokens->empty())
        {
            auto const inputDraftTokensLen = static_cast<SizeType32>(draftTokens->size());
            if (inputDraftTokensLen > maxDraftLen)
            {
                TLLM_THROW("Draft tokens length (%d) exceeds maximum draft tokens length (%d).", inputDraftTokensLen,
                    maxDraftLen);
            }
            draftLenPerEngineStep = inputDraftTokensLen;

            if (mPromptLen + draftLenPerEngineStep > maxInputLen)
            {
                auto const newDraftLenPerEngineStep = maxInputLen - mPromptLen;
                TLLM_LOG_WARNING(
                    "Prompt length + number of draft tokens (%d + %d) exceeds maximum input length (%d)."
                    "Number of draft tokens is changed to (%d)",
                    mPromptLen, draftLenPerEngineStep, maxInputLen, newDraftLenPerEngineStep);
                draftLenPerEngineStep = newDraftLenPerEngineStep;
                mDraftTokens->resize(draftLenPerEngineStep);
            }
        }

        if (mPromptLen + mMaxNewTokens + draftLenPerEngineStep > maxSequenceLen)
        {
            auto const maxNewTokens = maxSequenceLen - mPromptLen - draftLenPerEngineStep;
            TLLM_LOG_WARNING(
                "Prompt length + number of requested output tokens + draft tokens per step (%d + %d + %d) exceeds "
                "maximum sequence length (%d). "
                "Number of requested output tokens is changed to (%d).",
                mPromptLen, mMaxNewTokens, draftLenPerEngineStep, maxSequenceLen, maxNewTokens);
            mMaxNewTokens = maxNewTokens;
        }

        TLLM_CHECK_WITH_INFO(mSamplingConfig.validate(), "Incorrect sampling config");

        // validate extra ids when enabling kv cache reuse with prompt table
        if (enableKVCacheReuse && mPromptEmbeddingTable.has_value() && mPromptVocabSize.has_value())
        {
            TLLM_CHECK_WITH_INFO(mInputTokenExtraIds.has_value() && mInputTokenExtraIds.value(),
                "Input token extra ids must be provided when enabling kv cache reuse with prompt table");
            TLLM_CHECK_WITH_INFO(mInputTokenExtraIds.value()->size() == static_cast<size_t>(mOrigPromptLen),
                "inputTokenExtraIds vector size must be the same as input token vector size.");
        }
    }

    void setExcludeInputFromOutput(bool exclude)
    {
        mExcludeInputFromOutput = exclude;
    }

    /// @brief Get the params of the context
    /// @return The params of the context
    std::optional<executor::ContextPhaseParams> const& getContextPhaseParams() const noexcept
    {
        return mContextPhaseParams;
    }

    void setContextPhaseParams(executor::ContextPhaseParams contextPhaseParams)
    {
        mContextPhaseParams = std::move(contextPhaseParams);
    }

    /// @brief Get the state params of the context
    /// @return The state params of the context
    executor::ContextPhaseState const& getContextPhaseState() const
    {
        TLLM_CHECK(mContextPhaseParams.has_value());
        return *static_cast<executor::ContextPhaseState const*>(mContextPhaseParams.value().getState());
    }

    /// @brief Get total number of tokens for this req (prompt + generated)
    /// @param beam The beam index
    /// @return  The number of tokens
    [[nodiscard]] SizeType32 getNumTokens(SizeType32 beam) const
    {
        return mTokens.at(beam).size() - mNumPreDecodedTokens[beam];
    }

    /// @brief Get number of return sequences for this req.
    /// @return  The number of sequences to return.
    [[nodiscard]] SizeType32 getNumReturnSequences() const
    {
        return mNumReturnSequences;
    }

    /// @brief Get child requests spawned by this req.
    /// @return A vector of child requests.
    [[nodiscard]] std::vector<RequestPtr> const& getChildRequests() const
    {
        return mChildRequests;
    }

    /// @brief Get max number of tokens across all beams
    /// @return  The number of tokens
    [[nodiscard]] SizeType32 getMaxBeamNumTokens() const
    {
        SizeType32 maxTokens = 0;
        for (SizeType32 beam = 0; beam < mSamplingConfig.beamWidth; ++beam)
        {
            maxTokens = std::max(maxTokens, getNumTokens(beam));
        }
        return maxTokens;
    }

    /// @brief Get a token at a given position and beam index
    /// @param beam  The beam index
    /// @param pos The position of the token relative to beginning of the prompt
    /// @return  The token index
    [[nodiscard]] TokenIdType getToken(SizeType32 beam, SizeType32 pos) const
    {
        return mTokens.at(beam).at(pos);
    }

    /// @brief Get the tokens at a given beam index
    /// @param beam The beam index
    /// @return A vector of tokens for this beam index, includes the prompt
    [[nodiscard]] VecTokens const& getTokens(SizeType32 beam) const
    {
        return mTokens.at(beam);
    }

    /// @brief Get all tokens (input+output) for all beams
    /// @return A vector of vector of tokens.
    [[nodiscard]] BeamTokens const& getTokens() const
    {
        return mTokens;
    }

    /// @brief Get the unique tokens at a given beam index
    /// @param beam The beam index
    /// @return A vector of UniqueTokens for this beam index, includes the prompt
    [[nodiscard]] VecUniqueTokens const& getUniqueTokens(SizeType32 beam) const
    {
        return mUniqueTokens.at(beam);
    }

    /// @brief Get all unique tokens (input+output) for all beams
    /// @return A vector of vector of UniqueTokens.
    [[nodiscard]] BeamUniqueTokens const& getUniqueTokens() const
    {
        return mUniqueTokens;
    }

    /// @brief Get input tokens to encoder
    /// @return A vector of tokens.
    [[nodiscard]] std::optional<std::shared_ptr<VecTokens>> const& getEncoderTokens() const
    {
        return mEncoderTokens;
    }

    /// @brief Get the unique tokens to encoder
    /// @return A vector of UniqueTokens for encoder
    [[nodiscard]] std::optional<std::shared_ptr<VecUniqueTokens>> const& getEncoderUniqueTokens() const
    {
        return mEncoderUniqueTokens;
    }

    /// @brief Get length of encoder input (could be tokens or features length)
    /// @return An integer.
    [[nodiscard]] SizeType32 getEncoderInputLen() const
    {
        if (mEncoderInputFeatures.has_value())
        {
            return getEncoderInputFeatures()->getShape().d[0];
        }
        else if (getEncoderTokens().has_value())
        {
            return getEncoderTokens().value()->size();
        }
        else
        {
            TLLM_THROW("GenericLlmRequest::getEncoderInputLen - Do not have encoder length!");
        }
    }

    /// @brief Get length of encoder output. Fall back to encoder input length if not present
    /// @return An integer.
    [[nodiscard]] SizeType32 getEncoderOutputLen() const
    {
        if (mEncoderOutputLength.has_value())
        {
            return mEncoderOutputLength.value();
        }
        else
        {
            return getEncoderInputLen();
        }
    }

    [[nodiscard]] std::optional<std::shared_ptr<std::vector<SizeType32>>> getPositionIds() const
    {
        return mPositionIds;
    }

    /// @brief Get the draft tokens
    /// @return shared_ptr to vector of draft tokens
    [[nodiscard]] std::shared_ptr<VecTokens> const& getDraftTokens() const
    {
        return mDraftTokens;
    }

    /// @brief Get the logits for the draft tokens
    /// @return Tensor of draft logits
    [[nodiscard]] std::optional<TensorPtr> getDraftLogits() const
    {
        return mDraftLogits;
    }

    /// @brief Returns true if request has draft tokens
    /// @return flag
    [[nodiscard]] bool hasDraftTokens() const
    {
        return mDraftTokens && !mDraftTokens->empty();
    }

    /// @brief Get the maximum number of generated tokens among all rays in beam
    /// @return The number of generated tokens (doesn't include the prompt tokens)
    [[nodiscard]] SizeType32 getMaxNumGeneratedTokens() const
    {
        return getMaxBeamNumTokens() - mPromptLen;
    }

    [[nodiscard]] LlmRequestType getLlmRequestType() const
    {
        return mLlmRequestType;
    }

    /// @brief Add new generated tokens to the vector of tokens and set mLastTokens
    /// @param token The token to add
    /// @param beam The beam to which to add the new token
    void addNewToken(TokenIdType token, SizeType32 beam)
    {
        mLastTokens[beam] = token;
        mTokens.at(beam).push_back(token);
        // New token's extra id is 0
        mUniqueTokens.at(beam).push_back({token, 0});
    }

    /// @brief Add new generated tokens to the vector of tokens and set mLastTokens
    /// @param beamTokens A vector containing the tokens to add for each beam index
    ///                   beamTokens is expected to be of size beamWidth
    void addNewTokens(VecTokens const& beamTokens)
    {
        assert(static_cast<size_t>(mSamplingConfig.beamWidth) == beamTokens.size());
        mLastTokens = beamTokens;
        for (std::size_t beam = 0; beam < beamTokens.size(); ++beam)
        {
            auto const outputId = beamTokens[beam];
            mTokens.at(beam).push_back(outputId);
            // New token's extra id is 0
            mUniqueTokens.at(beam).push_back({outputId, 0});
        }
    }

    /// @brief Set the number of pre-decoded tokens
    /// @param num_tokens The number of pre-decoded tokens
    /// @param beam The beam to which to set the number of pre-decoded tokens
    void setNumPreDecodedTokens(SizeType32 num_tokens, SizeType32 beam)
    {
        mNumPreDecodedTokens[beam] = num_tokens;
    }

    /// @brief Sets the generated tokens for all beams after gatherTree. Erases all previous generated tokens.
    /// @param generatedBeamTokens The generated tokens for all beams (vector of vector of tokens)
    void setGeneratedTokens(BeamTokens const& generatedBeamTokens)
    {
        assert(generatedBeamTokens.size() == static_cast<size_t>(mSamplingConfig.beamWidth));
        for (std::size_t beam = 0; beam < generatedBeamTokens.size(); ++beam)
        {
            auto& beamTokens = mTokens[beam];
            beamTokens.resize(mPromptLen);
            beamTokens.insert(beamTokens.end(), generatedBeamTokens[beam].begin(), generatedBeamTokens[beam].end());
            auto& beamUniqueTokens = mUniqueTokens[beam];
            beamUniqueTokens.resize(mPromptLen);
            for (std::size_t i = 0; i < generatedBeamTokens[beam].size(); ++i)
            {
                beamUniqueTokens.push_back({generatedBeamTokens[beam][i], 0});
            }
        }
    }

    /// @brief Sets the number of return sequences.
    /// @param numReturnSequences The number of return sequences.
    void setNumReturnSequences(SizeType32 const& numReturnSequences)
    {
        TLLM_CHECK_WITH_INFO(!isChild(), "A child request cannot change numReturnSequences.");
        TLLM_CHECK_WITH_INFO(
            numReturnSequences > 0, "numReturnSequences should be a positive integer, got %d.", numReturnSequences);
        TLLM_CHECK_WITH_INFO(mChildRequests.size() <= static_cast<size_t>(numReturnSequences),
            "Cannot set numReturnSequences %d smaller than the number %ld of child requests that have already created.",
            numReturnSequences, mChildRequests.size());
        mNumReturnSequences = numReturnSequences;
        mSequenceFinalVec->resize(mNumReturnSequences);
    }

    [[nodiscard]] bool constexpr isChild() const noexcept
    {
        return mSequenceIndex > 0;
    }

    /// @brief Return a vector of the last-generated tokens of shape [num_beams]
    [[nodiscard]] VecTokens const& getLastTokens()
    {
        return mLastTokens;
    }

    /// @brief Return the last-generated token of from a particular beam
    [[nodiscard]] TokenIdType const& getLastTokens(SizeType32 beam)
    {
        return mLastTokens[beam];
    }

    /// @brief Pause a request by moving the generated tokens to the prompt
    /// @param maxInputLen The maximum prompt len.
    void pause(SizeType32 maxInputLen)
    {
        // TODO: For beamWidth > 1, we would need to support swapping to avoid
        // recomputing from the start
        // As a temporary solution, we currently reset the tokens to the prompt
        if (mSamplingConfig.beamWidth > 1)
        {
            for (std::size_t beam = 0; beam < mTokens.size(); ++beam)
            {
                auto& beamTokens = mTokens.at(beam);
                beamTokens.resize(mPromptLen);
                auto& beamUniqueTokens = mUniqueTokens.at(beam);
                beamUniqueTokens.resize(mPromptLen);
                if (returnLogProbs())
                {
                    mLogProbs.at(beam).clear();
                }
            }
        }
        else
        {
            SizeType32 newPromptLen = std::min(maxInputLen, mPromptLen + getMaxNumGeneratedTokens());
            for (std::size_t beam = 0; beam < mTokens.size(); ++beam)
            {
                auto& beamTokens = mTokens.at(beam);
                beamTokens.resize(newPromptLen);
                auto& beamUniqueTokens = mUniqueTokens.at(beam);
                beamUniqueTokens.resize(newPromptLen);

                if (returnLogProbs())
                {
                    auto& logProb = mLogProbs.at(beam);
                    logProb.resize(newPromptLen - mPromptLen);
                }
            }
            mMaxNewTokens -= (newPromptLen - mPromptLen);
            mPromptLen = newPromptLen;
        }

        // for enc-dec models, pause means saving generated tokens to prompt but need to re-do encoder phase
        mState = mEncoderTokens.has_value() || mEncoderInputFeatures ? REQUEST_STATE_ENCODER_INIT
                                                                     : REQUEST_STATE_CONTEXT_INIT;
        mContextCurrentPosition = 0;
        mContextChunkSize = std::nullopt;
        mSeqSlot.reset();
    }

    /// @brief Get the maximum length of tokens returned to the client. Use to ensure we don't return to
    /// client duplicated tokens.
    /// @return The maximum length of the tokens sent to the client.
    [[nodiscard]] SizeType32 getMaxSentTokenLen() const
    {
        return mMaxSentTokenLen;
    }

    /// @brief Sets the maximum length of tokens returned to the client. Use to ensure we don't return to
    /// client duplicated tokens.
    /// @param maxSentLength The new maximum length.
    void setMaxSentTokenLen(SizeType32 maxSentLength)
    {
        mMaxSentTokenLen = maxSentLength;
    }

    [[nodiscard]] std::optional<TensorPtr> getPromptEmbeddingTable() const
    {
        return mPromptEmbeddingTable;
    }

    [[nodiscard]] std::optional<SizeType32> getPromptVocabSize() const
    {
        return mPromptVocabSize;
    }

    [[nodiscard]] std::optional<LoraTaskIdType> getLoraTaskId() const
    {
        return mLoraTaskId;
    }

    void setLoraTaskId(LoraTaskIdType taskId)
    {
        mLoraTaskId = taskId;
    }

    [[nodiscard]] std::optional<TensorPtr> getLoraWeights() const
    {
        return mLoraWeights;
    }

    void setLoraWeights(TensorPtr weights)
    {
        mLoraWeights = weights;
    }

    void clearLoraWeights()
    {
        mLoraWeights = std::nullopt;
    }

    [[nodiscard]] std::optional<TensorPtr> getLoraConfig() const
    {
        return mLoraConfig;
    }

    void setLoraConfig(TensorPtr config)
    {
        mLoraConfig = config;
    }

    void clearLoraConfig()
    {
        mLoraConfig = std::nullopt;
    }

    [[nodiscard]] std::optional<executor::LookaheadDecodingConfig> getLookaheadConfig() const
    {
        return mLookaheadConfig;
    }

    void setLookaheadConfig(executor::LookaheadDecodingConfig config)
    {
        mLookaheadConfig = config;
    }

    void clearLookaheadConfig()
    {
        mLookaheadConfig = std::nullopt;
    }

    [[nodiscard]] std::optional<TensorPtr> getEmbeddingBias() const
    {
        return mEmbeddingBias;
    }

    [[nodiscard]] std::optional<TensorPtr> getBadWordsList() const
    {
        return mBadWordsList;
    }

    [[nodiscard]] std::optional<TensorPtr> getStopWordsList() const
    {
        return mStopWordsList;
    }

    [[nodiscard]] bool returnLogProbs() const
    {
        return mSamplingConfig.outputLogProbs.has_value() ? mSamplingConfig.outputLogProbs->at(0) : false;
    }

    void setReturnLogProbs(bool returnLogProbs)
    {
        mSamplingConfig.outputLogProbs = {{returnLogProbs}};
        mSamplingConfig.cumLogProbs = {{returnLogProbs}};
    }

    [[nodiscard]] std::vector<VecLogProbs> const& getLogProbs() const
    {
        return mLogProbs;
    }

    [[nodiscard]] VecLogProbs const& getLogProbs(SizeType32 beam) const
    {
        return mLogProbs.at(beam);
    }

    void setLogProbs(VecLogProbs const& logProbs, SizeType32 beam)
    {
        mLogProbs.at(beam).resize(mPromptLen - mOrigPromptLen);
        mLogProbs.at(beam).insert(mLogProbs.at(beam).end(), logProbs.begin(), logProbs.end());
    }

    [[nodiscard]] VecLogProbs const& getCumLogProbs() const
    {
        return mCumLogProbs;
    }

    void setCumLogProb(float cumLogProb, SizeType32 beam)
    {
        mCumLogProbs.at(beam) = cumLogProb;
    }

    [[nodiscard]] SizeType32 getOrigPromptLen() const
    {
        return mOrigPromptLen;
    }

    void setPrepopulatedPromptLen(SizeType32 prepopulatedPromptLen)
    {
        mPrepopulatedPromptLen = prepopulatedPromptLen;
    }

    [[nodiscard]] SizeType32 getPrepopulatedPromptLen() const
    {
        return mPrepopulatedPromptLen;
    }

    void setDraftTokens(std::shared_ptr<VecTokens> const& draftTokens)
    {
        mDraftTokens = draftTokens;
    }

    void setDraftLogits(std::optional<TensorPtr> const& draftLogits)
    {
        mDraftLogits = draftLogits;
    }

    [[nodiscard]] SizeType32 getNumDraftTokens() const
    {
        return mDraftTokens->size();
    }

    void discardDraftTokens(SizeType32 numTokensToDiscard)
    {
        TLLM_CHECK_WITH_INFO(
            numTokensToDiscard > 0, "Can only discard a positive amount of draft tokens, got %d", numTokensToDiscard);
        TLLM_CHECK_WITH_INFO(numTokensToDiscard <= getNumDraftTokens(),
            "Can't discard more draft tokens (%d) than exists (%d).", numTokensToDiscard, getNumDraftTokens());
        mDraftTokens->resize(getNumDraftTokens() - numTokensToDiscard);
    }

    void setNumTokensPerIteration(SizeType32 numTokensPerIteration)
    {
        mNumTokensPerIteration = std::max(1, numTokensPerIteration);
    }

    [[nodiscard]] SizeType32 getNumTokensPerIteration() const
    {
        return mNumTokensPerIteration;
    }

    void setReturnEncoderOutput(bool const returnEncoderOutput)
    {
        mReturnEncoderOutput = returnEncoderOutput;
    }

    [[nodiscard]] bool getReturnEncoderOutput() const
    {
        return mReturnEncoderOutput;
    }

    [[nodiscard]] TensorPtr const& getEncoderOutputHost() const
    {
        return mEncoderOutputHost;
    }

    [[nodiscard]] TensorPtr const getEncoderInputFeatures() const
    {
        return mEncoderInputFeatures.value_or(nullptr);
    }

    void setEncoderOutputHost(TensorPtr encoderOutputHost)
    {
        mEncoderOutputHost = std::move(encoderOutputHost);
    }

    void setEncoderOutput(TensorPtr encoderOutput)
    {
        mEncoderOutput = std::move(encoderOutput);
    }

    void allocEncoderOutputHost(SizeType32 encoderHiddenSize, nvinfer1::DataType dataType)
    {
        mEncoderOutputHost = runtime::BufferManager::pinned(
            runtime::ITensor::makeShape({getEncoderOutputLen(), encoderHiddenSize}), dataType);
    }

    [[nodiscard]] TensorPtr const& getEncoderOutput() const noexcept
    {
        return mEncoderOutput;
    }

    [[nodiscard]] TensorPtr const& getEncoderHiddenStates() const noexcept
    {
        return mEncoderHiddenStates;
    }

    void allocEncoderOutput(runtime::BufferManager const& manager, nvinfer1::DataType dataType)
    {
        // unique_ptr --> shared_ptr ownership move
        mEncoderOutput = std::move(manager.emptyTensor(runtime::MemoryType::kGPU, dataType));
    }

    void allocEncoderHiddenStates(runtime::BufferManager const& manager, nvinfer1::DataType dataType)
    {
        // unique_ptr --> shared_ptr ownership move
        mEncoderHiddenStates = std::move(manager.emptyTensor(runtime::MemoryType::kGPU, dataType));
    }

    void freeEncoderOutputBuffers()
    {
        TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);

        TLLM_LOG_DEBUG(
            "Encoder output buffers use count: %u, %u", mEncoderOutput.use_count(), mEncoderHiddenStates.use_count());

        // TODO: better ways to free shared_ptr buffers
        mEncoderOutput.reset();
        mEncoderHiddenStates.reset();

        TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
    }

    [[nodiscard]] bool constexpr isStreaming() const noexcept
    {
        return mIsStreaming;
    }

    void constexpr setStreaming(bool isStreaming) noexcept
    {
        mIsStreaming = isStreaming;
    }

    void setPriority(executor::PriorityType priority) noexcept
    {
        mPriority = priority;
    }

    void setReturnAllGeneratedTokens(bool const returnAllGeneratedTokens)
    {
        TLLM_CHECK_WITH_INFO(!mIsStreaming || mSamplingConfig.beamWidth == 1 || returnAllGeneratedTokens,
            "returnAllGeneratedTokens must be true if streaming AND beam search are used.");
        mReturnAllGeneratedTokens = returnAllGeneratedTokens;
    }

    [[nodiscard]] bool getReturnAllGeneratedTokens()
    {
        return mReturnAllGeneratedTokens;
    }

    void setReturnContextLogits(bool const returnContextLogits)
    {
        mReturnContextLogits = returnContextLogits;
    }

    [[nodiscard]] bool getReturnContextLogits() const
    {
        return mReturnContextLogits;
    }

    void setReturnGenerationLogits(bool const returnGenerationLogits)
    {
        TLLM_CHECK_WITH_INFO(!(mIsStreaming && mSamplingConfig.beamWidth > 1 && returnGenerationLogits),
            "returnGenerationLogits must be false if streaming AND beam search are used.");
        mReturnGenerationLogits = returnGenerationLogits;
    }

    [[nodiscard]] bool getReturnGenerationLogits() const
    {
        return mReturnGenerationLogits;
    }

    [[nodiscard]] TensorPtr const& getContextLogitsHost() const
    {
        return mContextLogitsHost;
    }

    /// @param contextLogitsHost Expected shape [promtLen, vocabSizePadded]
    void setContextLogitsHost(TensorPtr contextLogitsHost)
    {
        mContextLogitsHost = std::move(contextLogitsHost);
    }

    void allocContextLogitsHost(SizeType32 vocabSizePadded, nvinfer1::DataType logitsDataType)
    {
        mContextLogitsHost = runtime::BufferManager::pinnedPool(
            runtime::ITensor::makeShape({mPromptLen, vocabSizePadded}), logitsDataType);
    }

    [[nodiscard]] TensorPtr const& getGenerationLogitsHost() const
    {
        return mGenerationLogitsHost;
    }

    /// @param generationLogitsHost Expected shape
    /// * [beamWidth, maxNewTokens, vocabSizePadded] for non-speculative decoding
    /// * [1, numDraftTokens + 1, vocabSizePadded] for speculative decoding
    void setGenerationLogitsHost(TensorPtr generationLogitsHost)
    {
        mGenerationLogitsHost = std::move(generationLogitsHost);
    }

    void allocGenerationLogitsHost(SizeType32 vocabSizePadded, nvinfer1::DataType logitsDataType)
    {
        if (mIsStreaming)
        {
            // If streaming mode, the complete generation logits shape will be [1, beamWidth, vocabSizePadded],
            // or [allGeneratedTokens, beamWidth, vocabSizePadded] if mReturnAllGeneratedTokens is True.
            // This could reduce unnecessary format conversions and allows the data to be returned directly.
            mGenerationLogitsHost = runtime::BufferManager::pinnedPool(
                runtime::ITensor::makeShape({mMaxNewTokens, mSamplingConfig.beamWidth, vocabSizePadded}),
                logitsDataType);
        }
        else
        {
            mGenerationLogitsHost = runtime::BufferManager::pinnedPool(
                runtime::ITensor::makeShape({mSamplingConfig.beamWidth, mMaxNewTokens, vocabSizePadded}),
                logitsDataType);
        }
    }

    void allocTargetModelAcceptedTokenLogitsHost(SizeType32 vocabSizePadded, nvinfer1::DataType logitsDataType)
    {
        mGenerationLogitsHost = runtime::BufferManager::pinnedPool(
            runtime::ITensor::makeShape({1, getNumDraftTokens() + 1, vocabSizePadded}), logitsDataType);
    }

    [[nodiscard]] std::vector<TensorPtr> const& getGenerationLogitsFragments() const
    {
        return mGenerationLogitsFragments;
    }

    void addGenerationLogitsFragment(TensorPtr& genLogits)
    {
        mGenerationLogitsFragments.push_back(genLogits);
    }

    SizeType32 getGenerationLogitsFragmentsSize()
    {
        return mGenerationLogitsFragments.size();
    }

    void clearGenerationLogitsFragments()
    {
        mGenerationLogitsFragments.clear();
    }

    [[nodiscard]] bool hasReachedState(LlmRequestState_t state) const noexcept
    {
        return mState >= state;
    }

    [[nodiscard]] bool isEncoderInitState() const noexcept
    {
        return mState == REQUEST_STATE_ENCODER_INIT;
    }

    [[nodiscard]] bool isContextInitState() const noexcept
    {
        return mState == REQUEST_STATE_CONTEXT_INIT;
    }

    [[nodiscard]] bool isGenerationInProgressState() const noexcept
    {
        return mState == REQUEST_STATE_GENERATION_IN_PROGRESS || mState == REQUEST_STATE_GENERATION_TO_COMPLETE;
    }

    [[nodiscard]] bool isGenerationCompleteState() const noexcept
    {
        return mState == REQUEST_STATE_GENERATION_COMPLETE;
    }

    [[nodiscard]] bool isDisaggGenerationInitState() const noexcept
    {
        return mState == REQUEST_STATE_DISAGG_GENERATION_INIT;
    }

    [[nodiscard]] bool isDisaggContextTransmissionState() const noexcept
    {
        return mState == REQUEST_STATE_DISAGG_CONTEXT_TRANS_IN_PROGRESS;
    }

    [[nodiscard]] bool isDisaggContextCompleteState() const noexcept
    {
        return mState == REQUEST_STATE_DISAGG_CONTEXT_COMPLETE;
    }

    /// To determine whether the context is unchunked. When a context is chunked into only a part, it
    /// is still different from the unchunked state, which indicates the initial status.
    [[nodiscard]] bool isFullContextRequest() const noexcept
    {
        return (isContextInitState() || isDisaggGenerationInitState()) && !mContextChunkSize;
    }

    [[nodiscard]] bool isContextOnlyRequest() const noexcept
    {
        return mLlmRequestType == LlmRequestType::LLMREQUEST_TYPE_CONTEXT_ONLY;
    }

    void setContextCurrentPosition(SizeType32 contextCurrentPosition)
    {
        mContextCurrentPosition = contextCurrentPosition;
    }

    /// When chunked, the position of the current chunk is returned. Otherwise, only the beginning
    /// or end of the context is returned.
    [[nodiscard]] SizeType32 getContextCurrentPosition() const noexcept
    {
        return mContextCurrentPosition;
    }

    /// Return the length of the context that has not yet been processed.
    [[nodiscard]] SizeType32 getContextRemainingLength() const noexcept
    {
        return mPromptLen - getContextCurrentPosition();
    }

    /// To retrieve the context chunk size, throw an exception when the context is not chunked.
    [[nodiscard]] SizeType32 getContextChunkSize() const
    {
        TLLM_CHECK_WITH_INFO(
            isContextInitState() && mContextChunkSize, "The current request is not in context chunking state.");
        return mContextChunkSize.value();
    }

    /// To set the context chunk size, throw an exception when the chunk size is negative. If the chunk
    /// size is greater than the remaining length of the context, the size will be reduced to fit the
    /// remaining length.
    void setContextChunkSize(SizeType32 size)
    {
        TLLM_CHECK_WITH_INFO(isContextInitState(), "Chunking is only possible during the context phase.");
        TLLM_CHECK_WITH_INFO(size >= 0, "The chunk size of context (%d) can't be negative.", size);
        mContextChunkSize = std::min(size, getContextRemainingLength());
    }

    /// Determines whether the current position is only one chunk away from the end of the context.
    /// It will return true when the context is not chunked.
    [[nodiscard]] bool isLastContextChunk() const noexcept
    {
        return isFullContextRequest()
            || (isContextInitState() && getContextCurrentPosition() + getContextChunkSize() == mPromptLen);
    }

    /// Returns whether the position is at the beginning of the context. It will return true when the
    /// context is not chunked.
    [[nodiscard]] bool isFirstContextChunk() const noexcept
    {
        return isFullContextRequest() || getContextCurrentPosition() == 0;
    }

    [[nodiscard]] executor::PriorityType priority() const noexcept
    {
        return mPriority;
    }

    /// Move the cursor forward one chunk. When not chunked, move forward to the end of the context.
    void moveToNextContextChunk()
    {
        TLLM_CHECK_WITH_INFO(isContextInitState(), "Chunking is only possible during the context phase.");
        if (mContextChunkSize)
        {
            mContextCurrentPosition += getContextChunkSize();
            setContextChunkSize(0);
        }
        else
        {
            TLLM_CHECK_WITH_INFO(mContextCurrentPosition == 0, "Full context out of bounds.");
            mContextCurrentPosition = mPromptLen;
        }
    }

    /// Increment the counter of decoding iterations.
    void advanceDecodingIter()
    {
        mDecodingIter++;
    }

    /// @brief  Return the average number of decoded tokens per iteration. For standard model it is 1.
    /// For speculative decoding model >= 1 -- number of draft tokens accepted per step + 1.
    [[nodiscard]] float getAvgDecodedTokensPerIter() const noexcept
    {
        if (mDecodingIter == 0)
        {
            return 0.f;
        }
        return static_cast<float>(getMaxNumGeneratedTokens()) / mDecodingIter;
    }

    /// @brief  Create a Response from the current state of the request
    /// @return An optional Response
    std::optional<executor::Response> createResponse()
    {
        TLLM_CHECK(!isDisaggContextCompleteState());
        if (isGenerationCompleteState() || (mIsStreaming && isGenerationInProgressState())
            || isDisaggContextTransmissionState())
        {
            TLLM_LOG_DEBUG("Creating response for request %lu", mRequestId);

            executor::Result result;
            result.sequenceIndex = mSequenceIndex;

            result.isSequenceFinal = isGenerationCompleteState() || isDisaggContextTransmissionState();
            mSequenceFinalVec->at(mSequenceIndex) = result.isSequenceFinal;

            result.isFinal = std::all_of(mSequenceFinalVec->begin(), mSequenceFinalVec->end(),
                [](bool isSequenceFinal) { return isSequenceFinal; });

            auto const nbBeams = mSamplingConfig.beamWidth;
            auto const maxNbTokens = getMaxBeamNumTokens();

            if (isDisaggContextTransmissionState() && isContextOnlyRequest())
            {
                auto const reqBeamWidth = mSamplingConfig.beamWidth;
                std::vector<TokenIdType> firstGenTokens;
                for (SizeType32 beam = 0; beam < reqBeamWidth; ++beam)
                {
                    firstGenTokens.push_back(getTokens().at(beam).back());
                }
                // TODO: fill the rank ids
                result.contextPhaseParams = executor::ContextPhaseParams{
                    std::move(firstGenTokens), mContextPhaseParams.value().releaseState()};
            }

            auto const calculateNbTokensOut = [this](SizeType32 maxNbTokens)
            {
                if (!mIsStreaming)
                {
                    return maxNbTokens - (mExcludeInputFromOutput ? getOrigPromptLen() : 0);
                }
                return mReturnAllGeneratedTokens ? maxNbTokens - getOrigPromptLen()
                                                 : maxNbTokens - getMaxSentTokenLen();
            };

            auto const maxNbTokensOut = calculateNbTokensOut(maxNbTokens);

            result.outputTokenIds.resize(nbBeams);

            auto const startTokenPos = maxNbTokens - maxNbTokensOut;

            auto const shouldSendResponse = isGenerationCompleteState()
                || (mIsStreaming && maxNbTokens > getMaxSentTokenLen()) || isDisaggContextTransmissionState();

            if (!shouldSendResponse)
            {
                return std::nullopt;
            }
            else
            {
                for (SizeType32 beam = 0; beam < nbBeams; ++beam)
                {
                    auto const& tokens = getTokens(beam);
                    auto const nbTokensOut = calculateNbTokensOut(tokens.size());

                    if (nbTokensOut > 0)
                    {
                        auto const first = tokens.data() + startTokenPos;
                        result.outputTokenIds.at(beam).assign(first, first + nbTokensOut);
                    }
                }

                if (returnLogProbs())
                {
                    result.cumLogProbs = getCumLogProbs();
                    result.logProbs = getLogProbs();
                }

                if (getReturnContextLogits())
                {
                    result.contextLogits = executor::detail::ofITensor(getContextLogitsHost());
                }

                if (getReturnGenerationLogits())
                {
                    if (isStreaming())
                    {
                        auto startGenTokenPos = startTokenPos - getOrigPromptLen();
                        TensorPtr generationLogitsHostCurrentStep
                            = runtime::ITensor::slice(getGenerationLogitsHost(), startGenTokenPos, maxNbTokensOut);
                        result.generationLogits = executor::detail::ofITensor(generationLogitsHostCurrentStep);
                    }
                    else
                    {
                        result.generationLogits = executor::detail::ofITensor(getGenerationLogitsHost());
                    }
                }

                if (getReturnEncoderOutput())
                {
                    result.encoderOutput = executor::detail::ofITensor(getEncoderOutputHost());
                }

                result.finishReasons = mFinishReasons;
                result.decodingIter = mDecodingIter;

                // Update position of last sent response
                setMaxSentTokenLen(maxNbTokens);

                auto requestId = isChild() ? mParentRequestId : mRequestId;
                auto response = executor::Response(requestId, std::move(result));

                return response;
            }
        }
        else
        {
            return std::nullopt;
        }
    }

    void setFinishedReason(executor::FinishReason reason, SizeType32 beam)
    {
        mFinishReasons.at(beam) = reason;
    }

    void setDecodingIter(SizeType32 iter)
    {
        mDecodingIter = iter;
    }

    RequestIdType mRequestId;
    SizeType32 mPromptLen;
    SizeType32 mMaxNewTokens;
    // Tokens [beam_size, mPromptLen + getMaxNumGeneratedTokens()]
    runtime::SamplingConfig mSamplingConfig;
    LlmRequestState_t mState;
    std::optional<TokenIdType> mEndId;
    std::optional<TokenIdType> mPadId;
    std::optional<SizeType32> mSeqSlot;
    std::optional<LogitsPostProcessor> mLogitsPostProcessor;
    bool mApplyLogitsPostProcessorBatched;
    std::optional<RequestIdType> mClientId;
    // Position of mask token in GLM model inputs
    SizeType32 mMaskPosition{0};

protected:
    bool mIsStreaming;

    // A list of tokens generated at the current step.
    // Used to pass the decoded tokens as the input to the next step.
    // `mLastTokens[beam] != mTokens.back()[beam]` for streaming + beam search
    // as `mTokens` will be overwritten by the gathered tokens.
    VecTokens mLastTokens;
    BeamTokens mTokens;
    SizeType32 mOrigPromptLen;
    // A list of numbers of pre-deocded tokens on the last PP rank when using pipeline parallelism.
    // It is introduced as a WAR to solve the hanging problem caused by overestimating the used KV cache on the last PP
    // rank (because new tokens are decoded earlier). By excluding the numbers of pre-decoded tokens, the used KV cache
    // can be estimated correctly.
    std::vector<SizeType32> mNumPreDecodedTokens;
    // Number of tokens already in KV cache before context phase.
    // A value > 0 indicates cached KV cache blocks were reused.
    // Up to inputLen - 1 tokens can be reused.
    SizeType32 mPrepopulatedPromptLen{0};
    SizeType32 mMaxSentTokenLen;

    std::optional<TensorPtr> mEmbeddingBias;
    std::optional<TensorPtr> mBadWordsList;
    std::optional<TensorPtr> mStopWordsList;

    std::optional<std::shared_ptr<std::vector<SizeType32>>> mPositionIds;

    std::optional<TensorPtr> mPromptEmbeddingTable;
    std::optional<SizeType32> mPromptVocabSize;

    std::optional<LoraTaskIdType> mLoraTaskId;
    std::optional<TensorPtr> mLoraWeights;
    std::optional<TensorPtr> mLoraConfig;
    std::optional<executor::LookaheadDecodingConfig> mLookaheadConfig;

    // To enable chunked context, the FHMA paged kv-cache also needs to be enabled. Except for the last one,
    // the size of the context chunk needs to be an integer multiple of the kv-cache block size. The meaning
    // of null value is that the context is not chunked.
    std::optional<SizeType32> mContextChunkSize;
    SizeType32 mContextCurrentPosition;

    std::vector<VecLogProbs> mLogProbs; // [beamSize, seqLen]
    VecLogProbs mCumLogProbs;           // [beamSize]
    std::shared_ptr<VecTokens> mDraftTokens;
    std::optional<TensorPtr> mDraftLogits;
    SizeType32 mNumTokensPerIteration;

    // whether to return the full beams on each iteration. True when doing streaming + beamsearch
    bool mReturnAllGeneratedTokens;
    // Save logits
    bool mReturnContextLogits;
    bool mReturnGenerationLogits;
    bool mReturnLogProbs;
    TensorPtr mContextLogitsHost;    // [mPromptLen, vocab_size_padded]
    TensorPtr mGenerationLogitsHost; // [beam_size, mMaxNewTokens, vocab_size_padded]
    std::vector<TensorPtr> mGenerationLogitsFragments;

    bool mExcludeInputFromOutput;

    // Encoder-only and Encoder-Decoder models
    // Encoder input tokens
    std::optional<std::shared_ptr<VecTokens>> mEncoderTokens;
    bool mReturnEncoderOutput;
    // Encoder output, used to compute cross attention KV Cache
    TensorPtr mEncoderOutput;       // [numTokens, hidden_size]
    TensorPtr mEncoderHiddenStates; // for pipeline parallelism, [numTokens, hiddenSize]
    TensorPtr mEncoderOutputHost;

    SizeType32 mDecodingIter;
    executor::PriorityType mPriority;
    std::vector<executor::FinishReason> mFinishReasons;
    std::optional<TensorPtr> mEncoderInputFeatures; // Input features of encoder for multimodal models
    std::optional<SizeType32>
        mEncoderOutputLength; // For some models like Whisper, encoder output shape cannot be inferred from encoder
                              // input shape due to downsampling. Thus this is needed for setting buffer sizes correctly
    LlmRequestType mLlmRequestType;
    std::optional<executor::ContextPhaseParams> mContextPhaseParams;

    std::optional<std::shared_ptr<VecTokenExtraIds>> mInputTokenExtraIds;
    BeamUniqueTokens mUniqueTokens;
    // TODO: add real extra id for encoder tokens
    std::optional<std::shared_ptr<VecUniqueTokens>> mEncoderUniqueTokens;

    SizeType32 mNumReturnSequences;
    SizeType32 mSequenceIndex;
    std::vector<RequestPtr> mChildRequests;
    RequestIdType mParentRequestId;
    std::shared_ptr<std::vector<bool>> mSequenceFinalVec; // Indicators whether each sibling completes generation.

private:
    void initialize(VecTokens const& inputTokens, bool outputLogProbs)
    {
        // Scatter the input tokens to other beam
        mTokens = BeamTokens(mSamplingConfig.beamWidth, inputTokens);
        mLastTokens = VecTokens(mSamplingConfig.beamWidth);

        // Init mUniqueTokens
        VecUniqueTokens uniqueTokens;
        uniqueTokens.reserve(inputTokens.size());
        if (mInputTokenExtraIds.has_value() && mInputTokenExtraIds.value())
        {
            if (mInputTokenExtraIds.value()->size() != inputTokens.size())
            {
                std::string errStr = "inputTokenExtraIds vector size must be the same as input token vector size.";
                TLLM_THROW(errStr);
            }
            VecTokenExtraIds tokenExtraIds = *mInputTokenExtraIds.value();
            for (std::size_t i = 0; i < inputTokens.size(); ++i)
            {
                uniqueTokens.push_back({inputTokens[i], tokenExtraIds[i]});
            }
        }
        else
        {
            // Default extra id is 0
            for (std::size_t i = 0; i < inputTokens.size(); ++i)
            {
                uniqueTokens.push_back({inputTokens[i], 0});
            }
        }
        mUniqueTokens = BeamUniqueTokens(mSamplingConfig.beamWidth, uniqueTokens);

        // Init mEncoderUniqueTokens
        // TODO: use real extra id instead of default zero value
        if (mEncoderTokens.has_value() && mEncoderTokens.value())
        {
            auto const& encoderTokens = *(mEncoderTokens.value());
            auto encoderUniqueTokens = std::make_shared<VecUniqueTokens>();
            for (std::size_t i = 0; i < encoderTokens.size(); ++i)
            {
                encoderUniqueTokens->push_back({encoderTokens[i], 0});
            }
            mEncoderUniqueTokens = encoderUniqueTokens;
        }

        if ((mPromptEmbeddingTable.has_value() && !mPromptVocabSize.has_value())
            || (!mPromptEmbeddingTable.has_value() && mPromptVocabSize.has_value()))
        {
            std::string errStr
                = "Prompt embedding table and prompt vocab size tensors must both be provided for requests with "
                  "prompt "
                  "tuning enabled.";
            TLLM_THROW(errStr);
        }

        if (mDraftLogits.has_value() && mDraftTokens->empty())
        {
            TLLM_THROW("Draft tokens must be specified when draft logits are given.");
        }

        setReturnLogProbs(outputLogProbs);

        if (!isChild())
        {
            // Initialize result states unless it is a child and a child request should share parent's one.
            mSequenceFinalVec = std::make_shared<std::vector<bool>>(getNumReturnSequences(), false);
        }
    }

    TensorPtr createListTensor(std::list<VecTokens> const& wordsList)
    {
        std::vector<SizeType32> offsets;
        VecTokens words;
        SizeType32 offsetCnt = 0;
        for (auto const& tokens : wordsList)
        {
            offsetCnt += tokens.size();
            offsets.push_back(offsetCnt);
            words.insert(words.end(), tokens.begin(), tokens.end());
        }
        offsets.resize(words.size(), -1);

        SizeType32 numWords = static_cast<SizeType32>(words.size());
        auto shape = runtime::ITensor::makeShape({2, numWords});
        auto tensor = runtime::BufferManager::pinnedPool(shape, nvinfer1::DataType::kINT32);
        auto data = runtime::bufferCast<int32_t>(*tensor);
        std::memcpy(data, words.data(), numWords * sizeof(int32_t));
        std::memcpy(data + numWords, offsets.data(), numWords * sizeof(int32_t));

        // Add leading dim of 1
        tensor->unsqueeze(0);

        return tensor;
    }
};

class LlmRequest : public GenericLlmRequest<runtime::ITensor::SharedPtr>
{
public:
    using Base = GenericLlmRequest<runtime::ITensor::SharedPtr>;
    using TensorPtr = Base::TensorPtr;
    using SizeType32 = Base::SizeType32;
    using TokenIdType = Base::TokenIdType;
    using RequestIdType = Base::RequestIdType;
    using VecLogProbs = Base::VecLogProbs;
    using BeamTokens = Base::BeamTokens;
    using VecTokens = Base::VecTokens;
    using LoraTaskIdType = Base::LoraTaskIdType;
    using TokenExtraIdType = Base::TokenExtraIdType;
    using VecTokenExtraIds = Base::VecTokenExtraIds;

    LlmRequest(RequestIdType requestId, SizeType32 maxNewTokens, std::shared_ptr<VecTokens> inputTokens,
        runtime::SamplingConfig const& samplingConfig, bool isStreaming, std::optional<SizeType32> endId = std::nullopt,
        std::optional<SizeType32> padId = std::nullopt, std::optional<TensorPtr> embeddingBias = std::nullopt,
        std::optional<TensorPtr> badWordsList = std::nullopt, std::optional<TensorPtr> stopWordsList = std::nullopt,
        std::optional<std::shared_ptr<std::vector<SizeType32>>> positionIds = std::nullopt,
        std::optional<TensorPtr> promptEmbeddingTable = std::nullopt,
        std::optional<SizeType32> promptVocabSize = std::nullopt,
        std::optional<LoraTaskIdType> loraTaskId = std::nullopt, std::optional<TensorPtr> loraWeights = std::nullopt,
        std::optional<TensorPtr> loraConfig = std::nullopt,
        std::optional<executor::LookaheadDecodingConfig> lookaheadConfig = std::nullopt, bool returnLogProbs = false,
        bool returnContextLogits = false, bool returnGenerationLogits = false,
        std::optional<std::shared_ptr<VecTokens>> draftTokens = std::nullopt,
        std::optional<TensorPtr> draftLogits = std::nullopt, bool excludeInputFromOutput = false,
        std::optional<LogitsPostProcessor> logitsPostProcessor = std::nullopt,
        bool applyLogitsPostProcessorBatched = false,
        std::optional<std::shared_ptr<VecTokens>> encoderInputTokens = std::nullopt, bool returnEncoderOutput = false,
        std::optional<RequestIdType> clientId = std::nullopt,
        executor::PriorityType priority = executor::Request::kDefaultPriority,
        std::optional<TensorPtr> encoderInputFeatures = std::nullopt,
        std::optional<SizeType32> encoderOutputLength = std::nullopt,
        LlmRequestType llmRequestType = LlmRequestType::LLMREQUEST_TYPE_CONTEXT_AND_GENERATION,
        std::optional<std::shared_ptr<VecTokenExtraIds>> inputTokenExtraIds = std::nullopt,
        SizeType32 numReturnSequences = 1)
        : Base(requestId, maxNewTokens, std::move(inputTokens), samplingConfig, isStreaming, endId, padId,
            std::move(embeddingBias), std::move(badWordsList), std::move(stopWordsList), std::move(positionIds),
            std::move(promptEmbeddingTable), promptVocabSize, loraTaskId, std::move(loraWeights), std::move(loraConfig),
            std::move(lookaheadConfig), returnLogProbs, returnContextLogits, returnGenerationLogits,
            std::move(draftTokens), std::move(draftLogits), excludeInputFromOutput, std::move(logitsPostProcessor),
            applyLogitsPostProcessorBatched, std::move(encoderInputTokens), returnEncoderOutput, clientId, priority,
            std::move(encoderInputFeatures), std::move(encoderOutputLength), llmRequestType,
            std::move(inputTokenExtraIds), numReturnSequences)
    {
    }

    LlmRequest(RequestIdType requestId, executor::Request const& request,
        std::optional<Base::LogitsPostProcessor> logitsPostProcessor = std::nullopt,
        bool applyLogitsPostProcessorBatched = false)
        : Base(requestId, request)
    {
        mLogitsPostProcessor = std::move(logitsPostProcessor);
        mApplyLogitsPostProcessorBatched = applyLogitsPostProcessorBatched;
        mLookaheadConfig = request.getLookaheadConfig();
    }

    std::shared_ptr<LlmRequest> createChildRequest(RequestIdType requestId)
    {
        TLLM_CHECK_WITH_INFO(!isChild(), "A child request cannot create its own child.");
        TLLM_CHECK_WITH_INFO(mChildRequests.size() + 1 < static_cast<size_t>(getNumReturnSequences()),
            "Cannot create child requests more than the number of return sequences (%d)", getNumReturnSequences());
        auto childReq = std::make_shared<LlmRequest>(*this);
        childReq->mRequestId = requestId;
        childReq->mSequenceIndex = mChildRequests.size() + 1;
        childReq->mParentRequestId = this->mRequestId;
        childReq->mSequenceFinalVec = this->mSequenceFinalVec;
        childReq->mSeqSlot.reset();

        // To ensure different randomness across children, assign a unique random seed to each child
        // by adding its sequence index to the base seed. If no seed is provided, the parent's seed defaults to 0.
        using RandomSeedType = tensorrt_llm::executor::RandomSeedType;
        if (childReq->mSamplingConfig.randomSeed.has_value())
        {
            childReq->mSamplingConfig.randomSeed->at(0) += static_cast<RandomSeedType>(childReq->mSequenceIndex);
        }
        else
        {
            RandomSeedType defaultSeed{0};
            mSamplingConfig.randomSeed = std::vector<RandomSeedType>(1, defaultSeed);
            childReq->mSamplingConfig.randomSeed
                = std::vector<RandomSeedType>(1, defaultSeed + static_cast<RandomSeedType>(childReq->mSequenceIndex));
        }

        mChildRequests.push_back(childReq);
        return childReq;
    }

    void movePromptEmbeddingTableToGpu(runtime::BufferManager const& manager)
    {
        if (!mPromptEmbeddingTable.has_value()
            || mPromptEmbeddingTable.value()->getMemoryType() == runtime::MemoryType::kGPU)
        {
            return;
        }
        else
        {
            TensorPtr gpuPromptEmbeddingTable
                = manager.copyFrom(*mPromptEmbeddingTable.value(), runtime::MemoryType::kGPU);
            mPromptEmbeddingTable = gpuPromptEmbeddingTable;
        }
    }

    void moveLoraWeightsToGpu(runtime::BufferManager const& manager)
    {
        if (!mLoraWeights.has_value() || mLoraWeights.value()->getMemoryType() == runtime::MemoryType::kGPU)
        {
            return;
        }
        // TODO for tp / pp models we only need to move the bit that belong on the local device
        TensorPtr gpuLoraWeights = manager.copyFrom(*mLoraWeights.value(), runtime::MemoryType::kGPU);
        mLoraWeights = gpuLoraWeights;
    }
};

} // namespace tensorrt_llm::batch_manager
