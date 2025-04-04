/*
 * Copyright (c) OpenMMLab. All rights reserved.
 * Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2021, NAVER Corp.  Authored by CLOVA.
 * Copyright (c) 2022, SK Telecom Authored by A. Dialog
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

// Modified from
// https://github.com/NVIDIA/FasterTransformer/blob/main/src/fastertransformer/models/multi_gpu_gpt/ParallelGpt.cc

#include <algorithm>
#include <memory>

#include "src/turbomind/comm/device_comm.h"
#include "src/turbomind/macro.h"

#include "src/turbomind/models/llama/LlamaBatch.h"
#include "src/turbomind/models/llama/LlamaV2.h"
#include "src/turbomind/models/llama/LlamaWeight.h"
#include "src/turbomind/models/llama/SequenceManager.h"
#include "src/turbomind/models/llama/llama_params.h"
#include "src/turbomind/models/llama/llama_utils.h"
#include "src/turbomind/models/llama/unified_decoder.h"

#include "src/turbomind/kernels/gpt_kernels.h"

#include "src/turbomind/utils/Tensor.h"
#include "src/turbomind/utils/anomaly_handler.h"
#include "src/turbomind/utils/cuda_utils.h"
#include "src/turbomind/utils/logger.h"
#include "src/turbomind/utils/memory_utils.h"

namespace turbomind {

/// TODO: Padded vocab size should also be divisible by 8
inline int pad_vocab_size(int vocab_size, int tp)
{
    return (vocab_size + tp - 1) / tp * tp;
}

template<typename T>
LlamaV2<T>::LlamaV2(const ModelParam&               model,
                    const EngineParam&              engine,
                    const AttentionParam&           attn,
                    const MoeParam&                 moe,
                    const LoraParam&                lora,
                    const Context<T>&               ctx,
                    int                             max_batch_size,
                    std::shared_ptr<LlamaWeight<T>> weights):
    param_(model),
    attn_param_(attn),
    lora_param_(lora),
    comm_(&ctx.comm),
    tp_size_(engine.attn_tp_size),
    tp_rank_(engine.attn_tp_rank),
    head_num_(model.head_num),
    size_per_head_(model.head_dim),
    hidden_units_(model.hidden_units),
    layer_num_(model.layer_num),
    vocab_size_(model.vocab_size),
    vocab_size_padded_(pad_vocab_size(model.vocab_size, tp_size_)),
    rmsnorm_eps_(model.norm_eps),
    local_head_num_(model.head_num / engine.attn_tp_size),
    local_kv_head_num_(model.kv_head_num / engine.attn_tp_size),
    weights_(std::move(weights)),
    stream_(ctx.stream),
    cublas_wrapper_(ctx.cublas_wrapper.get()),
    allocator_(ctx.allocator.get()),
    linear_(ctx.linear.get()),
    is_free_buffer_after_forward_(false),
    debug_(isDebug())
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    if (comm_->d_comm && comm_->d_comm->Query(comm::kHasAllGather2D)) {
        use_allgather_2d_ = true;
    }

    unified_decoder_ = std::make_unique<UnifiedDecoder<T>>(model, engine, attn, moe, lora, ctx);

    dynamic_decode_layer_ = std::make_unique<DynamicDecodeLayer<T>>(vocab_size_,
                                                                    vocab_size_padded_,
                                                                    stream_,
                                                                    cublas_wrapper_,
                                                                    allocator_,
                                                                    is_free_buffer_after_forward_,
                                                                    (cudaDeviceProp*)&ctx.cuda_device_prop);

    unified_decoder_->allocateBuffer(max_batch_size);
}

template<typename T>
LlamaV2<T>::~LlamaV2()
{
    dynamic_decode_layer_.reset();
    unified_decoder_.reset();
}

template<typename T>
void LlamaV2<T>::updateEmbedding(T*               decoder_input,
                                 const int        bsz,
                                 const int*       h_input_length,
                                 const Sequence** sequences,
                                 int              token_num,
                                 int*             lora_mask,
                                 bool*            have_embeddings)
{
    if (isTuning())
        return;

    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    *have_embeddings          = false;
    int*             mask_ptr = nullptr;
    std::vector<int> mask;
    if (lora_mask != nullptr) {
        mask     = std::vector<int>(token_num);
        mask_ptr = mask.data();
    }

    for (int i = 0; i < bsz; i++) {
        const auto& seq        = *sequences[i];
        const auto& embeddings = seq.input_embeddings;
        const auto& ranges     = seq.input_embedding_ranges;
        for (int j = embeddings.size() - 1; j >= 0; j--) {
            int begin = ranges[j].first;
            int end   = ranges[j].second;
            if (seq.cache_len + h_input_length[i] - 1 < begin) {
                continue;
            }
            if (end <= seq.cache_len) {
                break;
            }
            int off_dst = std::max(0, begin - seq.cache_len);
            int off_src = std::max(0, seq.cache_len - begin);
            // calculate intersection of [begin, end) and [seq.cache_len, seq.cache_len + h_input_length[i])
            begin            = std::max(begin, seq.cache_len);
            end              = std::min(end, seq.cache_len + h_input_length[i]);
            size_t byte_size = (end - begin) * hidden_units_ * sizeof(T);
            T*     dst_ptr   = decoder_input + off_dst * hidden_units_;
            auto   src_ptr   = embeddings[j].data() + off_src * hidden_units_ * sizeof(T);
            cudaMemcpyAsync(dst_ptr, src_ptr, byte_size, cudaMemcpyDefault, stream_);
            if (lora_mask != nullptr) {
                std::fill_n(mask_ptr + off_dst, (end - begin), 1);
                *have_embeddings = true;
            }
        }
        decoder_input += h_input_length[i] * hidden_units_;
        mask_ptr += h_input_length[i];
    }

    if (lora_mask != nullptr && *have_embeddings) {
        cudaMemcpyAsync(lora_mask, mask.data(), sizeof(int) * token_num, cudaMemcpyDefault, stream_);
        cudaStreamSynchronize(stream_);
    }
    sync_check_cuda_error();
}

template<typename T>
void LlamaV2<T>::forwardUnified(T*               out,
                                T*               decoder_output,
                                T*               decoder_input,
                                void**           block_ptrs,
                                const int*       cu_block_cnts,
                                const int*       input_ids,
                                const int*       h_input_length,
                                const int*       h_context_length,
                                const float*     rope_theta,
                                const bool*      finished,
                                size_t           token_num,
                                const int*       local_token_nums,
                                int              dc_batch_size,
                                int              pf_batch_size,
                                int*             lora_mask,
                                const Sequence** sequences)
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    if (token_num) {
        if (tp_size_ == 1) {
            invokeInputIdsEmbeddingLookupPosEncoding(decoder_input,
                                                     nullptr,  // processed somewhere else
                                                     weights_->pre_decoder_embedding_table,
                                                     static_cast<T*>(nullptr),
                                                     pPromptTuningParam<T>{},
                                                     input_ids,
                                                     0,  // only used for position encoding
                                                     token_num,
                                                     token_num,
                                                     1,
                                                     hidden_units_,
                                                     stream_);
            sync_check_cuda_error();
        }
        else {
            const size_t local_hidden_units = hidden_units_ / tp_size_;
            const size_t slice              = token_num * local_hidden_units;
            invokeInputIdsEmbeddingLookupPosEncoding(decoder_output + tp_rank_ * slice,
                                                     nullptr,  // processed somewhere else
                                                     weights_->pre_decoder_embedding_table,
                                                     static_cast<T*>(nullptr),
                                                     pPromptTuningParam<T>{},
                                                     input_ids,
                                                     0,  // only used for position encoding
                                                     token_num,
                                                     token_num,
                                                     1,
                                                     local_hidden_units,
                                                     stream_);
            sync_check_cuda_error();

            comm_->d_comm->AllGather(decoder_output + tp_rank_ * slice,
                                     decoder_output,
                                     slice,
                                     getTensorType<T>(),
                                     comm_->d_tp_group,
                                     stream_);
            sync_check_cuda_error();

            invokeInPlaceTranspose102(
                decoder_input, decoder_output, tp_size_, token_num, local_hidden_units, false, stream_);

            sync_check_cuda_error();
        }

        count_and_fix(decoder_input, token_num * hidden_units_, "embedding", 1);
    }

    bool have_embeddings = false;
    if (token_num) {
        updateEmbedding(decoder_input,
                        dc_batch_size + pf_batch_size,
                        h_input_length,
                        sequences,
                        token_num,
                        lora_mask,
                        &have_embeddings);
        sync_check_cuda_error();
    }

    const auto   dtype = getTensorType<T>();
    const size_t bsz   = dc_batch_size + pf_batch_size;

    TensorMap inputs{
        {"decoder_input", {MEMORY_GPU, dtype, {token_num, hidden_units_}, decoder_input}},
        {"output_norm_weight", {MEMORY_GPU, dtype, {hidden_units_}, weights_->output_norm_weight}},
        {"h_q_len", {MEMORY_CPU, TYPE_INT32, {bsz}, h_input_length}},
        {"h_k_len", {MEMORY_CPU, TYPE_INT32, {bsz}, h_context_length}},
        {"finished", {MEMORY_GPU, TYPE_BOOL, {bsz}, finished}},
        {"dc_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &dc_batch_size}},
        {"pf_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &pf_batch_size}},
        {"rope_theta", {MEMORY_GPU, TYPE_FP32, {hidden_units_}, rope_theta}},
        {"cu_block_counts", {MEMORY_GPU, TYPE_INT32, {bsz}, cu_block_cnts}},
        {"local_token_nums", {MEMORY_GPU, TYPE_INT32, {1}, local_token_nums}},
    };

    TensorMap outputs{{"decoder_output", {MEMORY_GPU, dtype, {token_num, hidden_units_}, decoder_output}},
                      {"block_ptrs", {MEMORY_GPU, TYPE_UINT64, {bsz}, block_ptrs}},
                      {"last_token_hidden_units", {MEMORY_GPU, dtype, {bsz, hidden_units_}, out}}};

    if (lora_mask != nullptr && have_embeddings) {
        inputs.insert({"lora_mask", {MEMORY_GPU, TYPE_INT32, {token_num}, lora_mask}});
    }

    unified_decoder_->forward(&outputs, &inputs, &weights_->decoder_layer_weights);
}

template<typename T>
void LlamaV2<T>::postDecodeEmbedding(T* logits, T* local_logits, const T* decoder_output, int batch_size)
{
    NvtxScope scope("postDecodeEmbedding");
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);

    cudaDataType_t data_type = getCudaDataType<T>();
    float          alpha     = 1.f;
    float          beta      = 0.f;
    FT_CHECK(vocab_size_padded_ % tp_size_ == 0);
    const size_t local_vocab_size = vocab_size_padded_ / tp_size_;

    auto invoke_gemm = [&](int first, int n, auto C, size_t batch_stride_C, size_t rank_stride_C) {
        cublas_wrapper_->Gemm(CUBLAS_OP_T,
                              CUBLAS_OP_N,
                              local_vocab_size,  // m
                              n,
                              hidden_units_,  // k
                              &alpha,
                              weights_->post_decoder_embedding_kernel,
                              data_type,
                              hidden_units_,  // k
                              decoder_output + first * hidden_units_,
                              data_type,
                              hidden_units_,  // k
                              &beta,
                              C + first * batch_stride_C + tp_rank_ * rank_stride_C,
                              data_type,
                              batch_stride_C,  // ldc
                              CUDA_R_32F,
                              cublasGemmAlgo_t(-1));
    };

    if (tp_size_ == 1) {
        invoke_gemm(0, batch_size, logits, vocab_size_padded_, 0);
        sync_check_cuda_error();
    }
    else if (use_allgather_2d_ == false) {
        FT_CHECK(logits != local_logits);
        const size_t slice = batch_size * local_vocab_size;
        invoke_gemm(0, batch_size, local_logits, local_vocab_size, slice);
        sync_check_cuda_error();
        comm_->d_comm->AllGather(
            local_logits + tp_rank_ * slice, local_logits, slice, getTensorType<T>(), comm_->d_tp_group, stream_);
        sync_check_cuda_error();
        invokeTransposeAxis01(logits, local_logits, tp_size_, batch_size, local_vocab_size, stream_);
        sync_check_cuda_error();
    }
    else {
        FT_CHECK(logits == local_logits);
        const int max_stages       = 1;
        const int min_stage_tokens = 512;
        const int step = std::max(std::min(batch_size, min_stage_tokens), (batch_size + max_stages - 1) / max_stages);
        cudaStream_t comm_stream = stream_;
        cudaEvent_t  comm_event{};
        if (step < batch_size) {
            check_cuda_error(cudaStreamCreateWithFlags(&comm_stream, cudaStreamNonBlocking));
            check_cuda_error(cudaEventCreateWithFlags(&comm_event, cudaEventDisableTiming));
        }
        for (int first = 0; first < batch_size; first += step) {
            const int n = std::min(first + step, batch_size) - first;
            invoke_gemm(first, n, local_logits, vocab_size_padded_, local_vocab_size);
            sync_check_cuda_error();
            if (comm_stream != stream_) {
                check_cuda_error(cudaEventRecord(comm_event, stream_));
                check_cuda_error(cudaStreamWaitEvent(comm_stream, comm_event));
            }
            comm_->d_comm->AllGather2D(local_logits + first * vocab_size_padded_ + tp_rank_ * local_vocab_size,
                                       local_logits + first * vocab_size_padded_,
                                       vocab_size_padded_,
                                       local_vocab_size,
                                       local_vocab_size,
                                       n,
                                       getTensorType<T>(),
                                       {first == 0, first + n == batch_size},
                                       comm_->d_tp_group,
                                       comm_stream);
            sync_check_cuda_error();
        }
        if (comm_stream != stream_) {
            check_cuda_error(cudaEventRecord(comm_event, comm_stream));
            check_cuda_error(cudaStreamWaitEvent(stream_, comm_event));
            check_cuda_error(cudaEventDestroy(comm_event));
            check_cuda_error(cudaStreamDestroy(comm_stream));
        }
    }
}

template<typename T>
void LlamaV2<T>::dynamicDecode(int*            token_ids,
                               bool*           finished,
                               int*            sequence_length,
                               bool*           should_stop,
                               curandState_t*  curand_state,
                               TensorMap*      inputs,
                               TensorMap*      outputs,
                               const T*        logits,
                               const uint32_t* seq_limit_len,
                               const int*      context_length,
                               int             step,
                               int             ite,
                               size_t          max_context_len,
                               size_t          token_ids_len,
                               size_t          batch_size)
{
    NvtxScope scope("dynamicDecode");
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    int local_batch_size = (int)batch_size;

    std::unordered_map<std::string, Tensor> dynamic_decode_input_tensors{
        {"logits", {MEMORY_GPU, getTensorType<T>(), {batch_size, (size_t)1, vocab_size_padded_}, logits}},
        {"step", {MEMORY_CPU, TYPE_INT32, {1}, &step}},
        {"max_input_length", {MEMORY_CPU, TYPE_INT32, {1}, &max_context_len}},
        {"sequence_limit_length", {MEMORY_GPU, TYPE_UINT32, {batch_size}, seq_limit_len}},
        {"input_lengths", {MEMORY_GPU, TYPE_INT32, {batch_size, 1}, context_length}},
        {"ite", {MEMORY_CPU, TYPE_UINT32, {1}, &ite}},
        {"local_batch_size", {MEMORY_CPU, TYPE_INT32, {1}, &local_batch_size}},
    };

    const std::vector<std::string> optional_inputs{"end_ids",
                                                   "stop_words_list",
                                                   "bad_words_list",
                                                   "runtime_top_k",
                                                   "runtime_top_p",
                                                   "temperature",
                                                   "repetition_penalty"};
    for (const auto& key : optional_inputs) {
        if (inputs->isExist(key)) {
            dynamic_decode_input_tensors.insert({key, inputs->at(key)});
        }
    }

    std::unordered_map<std::string, Tensor> dynamic_decode_output_tensors{
        {"output_ids", {MEMORY_GPU, TYPE_INT32, {token_ids_len, batch_size, 1U}, token_ids}},
        {"finished", {MEMORY_GPU, TYPE_BOOL, {batch_size}, finished}},
        {"sequence_length", {MEMORY_GPU, TYPE_INT32, {batch_size}, sequence_length}},
        {"should_stop", {MEMORY_CPU, TYPE_BOOL, {1}, should_stop}},
        {"curand_state", {MEMORY_GPU, TYPE_VOID, {batch_size}, curand_state}}};

    const std::vector<std::string> optional_outputs{
        "cum_log_probs", "output_log_probs", "sampled_indexes", "sampled_logprobs", "sampled_nums"};
    for (const auto& key : optional_outputs) {
        if (outputs->isExist(key)) {
            dynamic_decode_output_tensors.insert({key, outputs->at(key)});
        }
    }

    dynamic_decode_layer_->forward(&dynamic_decode_output_tensors, &dynamic_decode_input_tensors);
}

template class LlamaV2<half>;
#ifdef ENABLE_FP32
template class LlamaV2<float>;
#endif
#ifdef ENABLE_BF16
template class LlamaV2<__nv_bfloat16>;
#endif

}  // namespace turbomind
