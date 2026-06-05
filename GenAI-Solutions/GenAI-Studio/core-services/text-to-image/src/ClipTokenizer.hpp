// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>

// ---------------------------------------------------------------------------
// ClipTokenizer
//
// C++ implementation of the OpenAI CLIP BPE tokenizer.
// Loads vocab.json and merges.txt from the model directory.
//
// Output: vector of 77 int32_t token IDs
//   [SOT, tok1, tok2, ..., EOT, 0, 0, ..., 0]
//   SOT = 49406  (<|startoftext|>)
//   EOT = 49407  (<|endoftext|>)
//   PAD = 0
// ---------------------------------------------------------------------------
class ClipTokenizer {
public:
    static constexpr int32_t SOT_TOKEN = 49406;
    static constexpr int32_t EOT_TOKEN = 49407;
    static constexpr int32_t PAD_TOKEN = 0;
    static constexpr int     MAX_LEN   = 77;

    ClipTokenizer() = default;

    // Load vocab.json and merges.txt from model_dir.
    // Throws std::runtime_error on failure.
    void load(const std::string& model_dir);

    bool isLoaded() const noexcept { return loaded_; }

    // Tokenize text → vector of MAX_LEN int32_t token IDs.
    // Pads with PAD_TOKEN, truncates if necessary.
    std::vector<int32_t> tokenize(const std::string& text) const;

    // Tokenize and return as float32 (required by text encoder input)
    std::vector<float> tokenizeAsFloat(const std::string& text) const;

    // Return empty-string tokens (for unconditional guidance)
    std::vector<int32_t> emptyTokens() const;
    std::vector<float>   emptyTokensAsFloat() const;

private:
    // BPE encode a single word (already split into characters)
    std::vector<std::string> bpeEncode(const std::string& word) const;

    // Get all adjacent pairs in a sequence of tokens
    static std::vector<std::pair<std::string,std::string>>
    getPairs(const std::vector<std::string>& tokens);

    // Preprocess text: lowercase, add spaces around punctuation
    static std::string preprocess(const std::string& text);

    // Split preprocessed text into words
    static std::vector<std::string> splitWords(const std::string& text);

    std::unordered_map<std::string, int32_t> vocab_;   // token → id
    std::map<std::pair<std::string,std::string>, int>  merges_; // pair → rank
    bool loaded_ = false;
};
