// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace App::ModelCatalog {
struct LocalModelEntry {
    std::string key;
    std::string model_id;
    std::string model_dir;
    std::string config_path;
    bool active{false};
};

std::string ModelIdFromConfigJson(const std::string& config_json,
                                  const std::string& fallback = "genie");
std::string AliasModelIdForConfigPath(const std::string& config_path,
                                      const std::string& parsed_model_id);
std::string InferModelFamily(const std::string& config_path,
                             const std::string& model_id);
std::string DefaultPromptTemplateForModel(const std::string& config_path,
                                          const std::string& model_id);
std::vector<std::string> TextModelRoots();
std::vector<LocalModelEntry> DiscoverLocalModels(const std::string& active_config_path,
                                                 const std::string& active_model_id);
bool ResolveModelConfigPath(const nlohmann::json& body,
                            std::string& resolved_config_path,
                            std::string& resolved_model_key,
                            std::string& err);
bool NormalizeRequestedModelName(const std::string& requested_model,
                                 const std::string& active_model_id,
                                 std::string& normalized_model);
} // namespace App::ModelCatalog
