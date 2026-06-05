// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <set>
#include <string>
#include <vector>

namespace asr::catalog {

const std::vector<std::string>& transcriptionModels();
const std::set<std::string>& validTranscriptionModels();
bool normalizeTranscriptionModel(const std::string& requested_model,
                                 std::string& normalized_model);

const std::vector<std::string>& translationModels();
const std::set<std::string>& validTranslationModels();
bool normalizeTranslationModel(const std::string& requested_model,
                               std::string& normalized_model);

}  // namespace asr::catalog
