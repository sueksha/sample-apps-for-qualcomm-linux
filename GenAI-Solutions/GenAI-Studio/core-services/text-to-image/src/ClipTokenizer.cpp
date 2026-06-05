// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ClipTokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// Minimal JSON parser for vocab.json
// vocab.json format: { "token": id, ... }
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, int32_t> parseVocabJson(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open vocab.json: " + path);
    nlohmann::json j;
    f >> j;
    if (!j.is_object()) {
        throw std::runtime_error("Invalid vocab.json format: expected object");
    }
    std::unordered_map<std::string, int32_t> vocab;
    vocab.reserve(j.size());
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_number_integer()) continue;
        vocab.emplace(it.key(), static_cast<int32_t>(it.value().get<int64_t>()));
    }
    return vocab;
}

// ---------------------------------------------------------------------------
// Load merges.txt
// Format: one merge per line "token_a token_b"
// First line is a comment (#version ...)
// ---------------------------------------------------------------------------
static std::map<std::pair<std::string,std::string>, int>
parseMergesTxt(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open merges.txt: " + path);

    std::map<std::pair<std::string,std::string>, int> merges;
    std::string line;
    int rank = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string a = line.substr(0, sp);
        std::string b = line.substr(sp + 1);
        // strip trailing whitespace
        while (!b.empty() && (b.back() == '\r' || b.back() == '\n' || b.back() == ' '))
            b.pop_back();
        merges[{a, b}] = rank++;
    }
    return merges;
}

// ---------------------------------------------------------------------------
// ClipTokenizer::load
// ---------------------------------------------------------------------------
void ClipTokenizer::load(const std::string& model_dir) {
    std::string dir = model_dir;
    if (!dir.empty() && dir.back() != '/') dir += '/';

    vocab_  = parseVocabJson(dir + "vocab.json");
    merges_ = parseMergesTxt(dir + "merges.txt");
    loaded_ = true;

    std::cout << "[ClipTokenizer] Loaded " << vocab_.size()
              << " tokens, " << merges_.size() << " merges\n";
}

// ---------------------------------------------------------------------------
// Preprocess: lowercase + add spaces around punctuation
// ---------------------------------------------------------------------------
/*static*/
std::string ClipTokenizer::preprocess(const std::string& text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        if (std::ispunct(c) && c != '\'') {
            out += ' ';
            out += static_cast<char>(std::tolower(c));
            out += ' ';
        } else {
            out += static_cast<char>(std::tolower(c));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Split into words (whitespace-separated)
// ---------------------------------------------------------------------------
/*static*/
std::vector<std::string> ClipTokenizer::splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream ss(text);
    std::string w;
    while (ss >> w) words.push_back(w);
    return words;
}

// ---------------------------------------------------------------------------
// getPairs – all adjacent pairs in a token sequence
// ---------------------------------------------------------------------------
/*static*/
std::vector<std::pair<std::string,std::string>>
ClipTokenizer::getPairs(const std::vector<std::string>& tokens) {
    std::vector<std::pair<std::string,std::string>> pairs;
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        pairs.push_back({tokens[i], tokens[i + 1]});
    return pairs;
}

// ---------------------------------------------------------------------------
// bpeEncode – BPE-encode a single word
// The last character gets the </w> suffix (CLIP convention).
// ---------------------------------------------------------------------------
std::vector<std::string> ClipTokenizer::bpeEncode(const std::string& word) const {
    if (word.empty()) return {};

    // Start: each character is a token; last char gets </w>
    std::vector<std::string> tokens;
    for (size_t i = 0; i < word.size(); ++i) {
        std::string ch(1, word[i]);
        if (i + 1 == word.size()) ch += "</w>";
        tokens.push_back(ch);
    }

    if (tokens.size() == 1) return tokens;

    // Iteratively merge the highest-priority pair
    while (tokens.size() > 1) {
        auto pairs = getPairs(tokens);

        // Find the pair with the lowest rank (highest priority)
        int best_rank = std::numeric_limits<int>::max();
        std::pair<std::string,std::string> best_pair;
        bool found = false;

        for (const auto& p : pairs) {
            auto it = merges_.find(p);
            if (it != merges_.end() && it->second < best_rank) {
                best_rank = it->second;
                best_pair = p;
                found = true;
            }
        }

        if (!found) break;  // no more merges possible

        // Apply the merge
        std::vector<std::string> new_tokens;
        size_t i = 0;
        while (i < tokens.size()) {
            if (i + 1 < tokens.size() &&
                tokens[i] == best_pair.first &&
                tokens[i + 1] == best_pair.second) {
                new_tokens.push_back(best_pair.first + best_pair.second);
                i += 2;
            } else {
                new_tokens.push_back(tokens[i]);
                ++i;
            }
        }
        tokens = std::move(new_tokens);
    }

    return tokens;
}

// ---------------------------------------------------------------------------
// tokenize
// ---------------------------------------------------------------------------
std::vector<int32_t> ClipTokenizer::tokenize(const std::string& text) const {
    std::vector<int32_t> ids;
    ids.reserve(MAX_LEN);
    ids.push_back(SOT_TOKEN);

    if (!text.empty()) {
        const std::string processed = preprocess(text);
        const auto words = splitWords(processed);

        for (const auto& word : words) {
            const auto bpe_tokens = bpeEncode(word);
            for (const auto& tok : bpe_tokens) {
                if (static_cast<int>(ids.size()) >= MAX_LEN - 1) goto done;
                auto it = vocab_.find(tok);
                if (it != vocab_.end()) {
                    ids.push_back(it->second);
                } else {
                    // Unknown token → use UNK (id 49407 - 1 = 49406 is SOT, use 0)
                    ids.push_back(PAD_TOKEN);
                }
            }
        }
    }

done:
    // Truncate to MAX_LEN - 1 to leave room for EOT
    if (static_cast<int>(ids.size()) >= MAX_LEN)
        ids.resize(MAX_LEN - 1);

    ids.push_back(EOT_TOKEN);

    // Pad to MAX_LEN
    while (static_cast<int>(ids.size()) < MAX_LEN)
        ids.push_back(PAD_TOKEN);

    return ids;
}

std::vector<float> ClipTokenizer::tokenizeAsFloat(const std::string& text) const {
    const auto ids = tokenize(text);
    std::vector<float> out(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        out[i] = static_cast<float>(ids[i]);
    return out;
}

std::vector<int32_t> ClipTokenizer::emptyTokens() const {
    return tokenize("");
}

std::vector<float> ClipTokenizer::emptyTokensAsFloat() const {
    return tokenizeAsFloat("");
}
