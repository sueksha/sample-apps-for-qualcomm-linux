// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ModelCatalog.hpp"

#include <unordered_map>

namespace asr::catalog {
namespace {
constexpr const char* kCanonicalWhisperTiny = "whisper-tiny";

const std::vector<std::string> kTranscriptionModelCatalog = {
    kCanonicalWhisperTiny,
    "whisper-1",
    "gpt-4o-transcribe",
};

const std::set<std::string> kValidTranscriptionModels(
    kTranscriptionModelCatalog.begin(),
    kTranscriptionModelCatalog.end());

const std::unordered_map<std::string, std::string> kTranscriptionModelAliases = {
    {kCanonicalWhisperTiny, kCanonicalWhisperTiny},
    {"whisper-1", kCanonicalWhisperTiny},
    {"gpt-4o-transcribe", kCanonicalWhisperTiny},
};

const std::vector<std::string> kTranslationModelCatalog = {
    kCanonicalWhisperTiny,
    "whisper-1",
};

const std::set<std::string> kValidTranslationModels(
    kTranslationModelCatalog.begin(),
    kTranslationModelCatalog.end());

const std::unordered_map<std::string, std::string> kTranslationModelAliases = {
    {kCanonicalWhisperTiny, kCanonicalWhisperTiny},
    {"whisper-1", kCanonicalWhisperTiny},
};
}  // namespace

const std::vector<std::string>& transcriptionModels() {
    return kTranscriptionModelCatalog;
}

const std::set<std::string>& validTranscriptionModels() {
    return kValidTranscriptionModels;
}

bool normalizeTranscriptionModel(const std::string& requested_model,
                                 std::string& normalized_model) {
    const auto it = kTranscriptionModelAliases.find(requested_model);
    if (it == kTranscriptionModelAliases.end()) return false;
    normalized_model = it->second;
    return true;
}

const std::vector<std::string>& translationModels() {
    return kTranslationModelCatalog;
}

const std::set<std::string>& validTranslationModels() {
    return kValidTranslationModels;
}

bool normalizeTranslationModel(const std::string& requested_model,
                               std::string& normalized_model) {
    const auto it = kTranslationModelAliases.find(requested_model);
    if (it == kTranslationModelAliases.end()) return false;
    normalized_model = it->second;
    return true;
}

}  // namespace asr::catalog
