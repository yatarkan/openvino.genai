// Copyright (C) 2023-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <openvino/openvino.hpp>
#include "sampling_parameters.hpp"


using GenerationResult = std::vector<int64_t>;

class LLMEngine {
    ov::InferRequest m_model_runner;

    GenerationResult greedy_search(ov::Tensor prompts, SamplingParameters sampling_params) {
        ov::Shape prompts_shape = prompts.get_shape();
        size_t batch_size = prompts_shape[0];
        OPENVINO_ASSERT(batch_size == 1);
        
        GenerationResult results;
        results.reserve(sampling_params.max_new_tokens);
        auto attention_mask = ov::Tensor{ov::element::i64, prompts.get_shape()};
        std::fill_n(attention_mask.data<int64_t>(), attention_mask.get_size(), 1);
        auto position_ids = ov::Tensor{ov::element::i64, prompts.get_shape()};
        std::iota(position_ids.data<int64_t>(), position_ids.data<int64_t>() + position_ids.get_size(), 0);
        auto initial_seq_len = prompts.get_shape()[1];

        m_model_runner.set_tensor("input_ids", prompts);
        m_model_runner.set_tensor("attention_mask", attention_mask);
        m_model_runner.set_tensor("position_ids", position_ids);
    
        // set beam_idx for stateful model: no beam search is used and BATCH_SIZE = 1
        m_model_runner.get_tensor("beam_idx").set_shape({batch_size});
        m_model_runner.get_tensor("beam_idx").data<int32_t>()[0] = 0;

        for (size_t i = 0; i < sampling_params.max_new_tokens; ++i) {
            m_model_runner.infer();
            auto logits = m_model_runner.get_tensor("logits");
            ov::Shape logits_shape = logits.get_shape();

            size_t batch_size = logits_shape[0], seq_len = logits_shape[1], vocab_size = logits_shape[2];
            OPENVINO_ASSERT(batch_size == 1);
            // todo: implement for batch > 1

            const float * logits_data = logits.data<const float>() + (seq_len - 1) * vocab_size;
            int64_t out_token = std::max_element(logits_data, logits_data + vocab_size) - logits_data;

            m_model_runner.get_tensor("input_ids").set_shape({batch_size, 1});
            m_model_runner.get_tensor("input_ids").data<int64_t>()[0] = out_token;
            
            m_model_runner.get_tensor("attention_mask").set_shape({batch_size, m_model_runner.get_tensor("attention_mask").get_shape()[1] + 1});
            std::fill_n(m_model_runner.get_tensor("attention_mask").data<int64_t>(), m_model_runner.get_tensor("attention_mask").get_size(), 1);
            
            m_model_runner.get_tensor("position_ids").set_shape({batch_size, 1});
            m_model_runner.get_tensor("position_ids").data<int64_t>()[0] = int64_t(initial_seq_len + i);
            results.emplace_back(out_token);
        }
        return results;
    }

    GenerationResult beam_search(ov::Tensor prompts, SamplingParameters sampling_params) {
        // todo: implement
        GenerationResult results;
        results.reserve(10);
        return results;
    }

    GenerationResult multinomial_sampling(ov::Tensor prompts, SamplingParameters sampling_params) {
        // todo: implement
        GenerationResult results;
        results.reserve(10);
        return results;
    }

public:
    LLMEngine(ov::InferRequest& request) :
          m_model_runner(request) {
            // todo
    }
    
    LLMEngine() = default;

    // more high level interface
    GenerationResult generate(ov::Tensor prompts, SamplingParameters sampling_params) {
        if (sampling_params.is_gready_sampling()) {
            return greedy_search(prompts, sampling_params);
        } else if (sampling_params.is_beam_search()) {
            return beam_search(prompts, sampling_params);
        } else {  // if (sampling_params.is_multimomial()) {
            return multinomial_sampling(prompts, sampling_params);
        }
    }
};

std::pair<ov::Tensor, ov::Tensor> tokenize(ov::InferRequest& tokenizer, std::string prompt) {
    size_t batch_size = 1;
    tokenizer.set_input_tensor(ov::Tensor{ov::element::string, {batch_size}, &prompt});
    tokenizer.infer();
    return {tokenizer.get_tensor("input_ids"), tokenizer.get_tensor("attention_mask")};
}

std::pair<ov::Tensor, ov::Tensor> tokenize(ov::InferRequest& tokenizer, std::vector<std::string> prompts) {
    tokenizer.set_input_tensor(ov::Tensor{ov::element::string, {prompts.size()}, &prompts});
    tokenizer.infer();
    return {tokenizer.get_tensor("input_ids"), tokenizer.get_tensor("attention_mask")};
}

std::string detokenize(ov::InferRequest& detokenizer, std::vector<int64_t> tokens) {
    size_t batch_size = 1;
    detokenizer.set_input_tensor(ov::Tensor{ov::element::i64, {batch_size, tokens.size()}, tokens.data()});
    detokenizer.infer();
    return detokenizer.get_output_tensor().data<std::string>()[0];
}

std::vector<std::string> detokenize(ov::InferRequest& detokenizer, ov::Tensor tokens) {
    detokenizer.set_input_tensor(tokens);
    detokenizer.infer();
    auto res = detokenizer.get_output_tensor();
    
    std::vector<std::string> strings;
    strings.reserve(res.get_shape()[0]);
    for (int i = 0; i < res.get_shape()[0]; ++i) {
        strings.emplace_back(res.data<std::string>()[i]);
    }
    return strings;
}


// The following reasons require TextStreamer to keep a cache of previous tokens:
// detokenizer removes starting ' '. For example detokenize(tokenize(" a")) == "a",
// but detokenize(tokenize("prefix a")) == "prefix a"
// 1 printable token may consist of 2 token ids: detokenize(incomplete_token_idx) == "�"
struct TextStreamer {
    ov::InferRequest detokenizer;
    std::vector<int64_t> token_cache;
    size_t print_len = 0;

    void put(int64_t token) {
        token_cache.push_back(token);
        std::string text = detokenize(detokenizer, token_cache);
        if (!text.empty() && '\n' == text.back()) {
            // Flush the cache after the new line symbol
            std::cout << std::string_view{text.data() + print_len, text.size() - print_len};
            token_cache.clear();
            print_len = 0;
            return;
        }
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, "�") == 0) {
            // Don't print incomplete text
            return;
        }
        std::cout << std::string_view{text.data() + print_len, text.size() - print_len} << std::flush;
        print_len = text.size();
    }

    void end() {
        std::string text = detokenize(detokenizer, token_cache);
        std::cout << std::string_view{text.data() + print_len, text.size() - print_len} << '\n';
        token_cache.clear();
        print_len = 0;
    }
};

class LLMPipeline {
    LLMEngine m_model_runner;
    ov::InferRequest m_tokenizer;
    ov::InferRequest m_detokenizer;
    std::string m_path;
    SamplingParameters m_sampling_parameters;

public:
    LLMPipeline(std::string& path) : m_path(path) {
        if (std::filesystem::exists(m_path + "/generation_config.json")) {
            m_sampling_parameters = SamplingParameters(m_path + "/generation_config.json");
        }

        ov::Core core;
        // The model can be compiled for GPU as well
        auto model_request = core.compile_model(m_path + "/openvino_model.xml", "CPU").create_infer_request();
        m_model_runner = LLMEngine(model_request);

        // tokenizer and detokenizer work on CPU only
        core.add_extension(OPENVINO_TOKENIZERS_PATH);  // OPENVINO_TOKENIZERS_PATH is defined in CMakeLists.txt
        m_tokenizer = core.compile_model(m_path + "/openvino_tokenizer.xml", "CPU").create_infer_request();
        m_detokenizer = core.compile_model(m_path + "/openvino_detokenizer.xml", "CPU").create_infer_request();
    }

    std::string call(std::string text) {
        auto [input_ids, attention_mask] = tokenize(m_tokenizer, text);

        auto generate_results = m_model_runner.generate(input_ids, m_sampling_parameters);

        return detokenize(m_detokenizer, generate_results);
    }
};