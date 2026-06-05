// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "server/ModelCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <vector>

#include "server/ServerUtils.hpp"

using json = nlohmann::json;

namespace {
void AddUniqueNormalizedPath(std::vector<std::string>& paths,
                             std::set<std::string>& seen,
                             const std::string& raw_path) {
    if (raw_path.empty()) return;

    const std::string normalized = App::ServerUtils::NormalizePath(raw_path);
    if (normalized.empty()) return;
    if (seen.insert(normalized).second) {
        paths.push_back(normalized);
    }
}

bool IsRegularFilePath(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) &&
           std::filesystem::is_regular_file(path, ec);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

App::ModelCatalog::LocalModelEntry BuildLocalModelEntry(
    const std::string& normalized_config_path,
    const std::string& active_config_path,
    const std::string& active_model_id,
    bool force_active,
    bool alias_on_read_failure) {
    App::ModelCatalog::LocalModelEntry model;

    const std::filesystem::path config_path(normalized_config_path);
    const std::filesystem::path model_dir = config_path.parent_path();
    model.key = model_dir.filename().string();
    model.model_dir = App::ServerUtils::NormalizePath(model_dir.string());
    model.config_path = normalized_config_path;

    std::string config_json;
    std::string read_err;
    const bool loaded =
        App::ServerUtils::ReadFileToString(normalized_config_path, config_json, read_err);

    if (loaded) {
        model.model_id = App::ModelCatalog::AliasModelIdForConfigPath(
            normalized_config_path,
            App::ModelCatalog::ModelIdFromConfigJson(config_json, model.key));
    } else if (alias_on_read_failure) {
        model.model_id =
            App::ModelCatalog::AliasModelIdForConfigPath(normalized_config_path, model.key);
    } else {
        model.model_id = model.key;
    }

    model.active = force_active ||
                   (!active_config_path.empty() &&
                    normalized_config_path == active_config_path) ||
                   (active_config_path.empty() && model.model_id == active_model_id);

    return model;
}

std::vector<std::string> BuildCandidateConfigPaths(const json& body,
                                                   std::string& resolved_model_key) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;

    std::string config_path = App::ServerUtils::SafeStr(body, "genie_config_path", "");
    if (config_path.empty()) {
        config_path = App::ServerUtils::SafeStr(body, "model_config_path", "");
    }
    if (config_path.empty()) {
        config_path = App::ServerUtils::SafeStr(body, "config_path", "");
    }
    AddUniqueNormalizedPath(candidates, seen, config_path);

    const std::string model_dir = App::ServerUtils::SafeStr(body, "model_dir", "");
    if (!model_dir.empty()) {
        AddUniqueNormalizedPath(
            candidates,
            seen,
            (std::filesystem::path(model_dir) / "genie_config.json").string());
    }

    std::string model_key = App::ServerUtils::SafeStr(body, "model", "");
    if (model_key.empty()) {
        model_key = App::ServerUtils::SafeStr(body, "model_name", "");
    }
    if (model_key.empty()) {
        return candidates;
    }

    resolved_model_key = model_key;

    const std::filesystem::path maybe_path(model_key);
    if (model_key.find('/') != std::string::npos ||
        model_key.find('.') != std::string::npos || maybe_path.is_absolute()) {
        if (maybe_path.extension() == ".json") {
            AddUniqueNormalizedPath(candidates, seen, maybe_path.string());
        } else {
            AddUniqueNormalizedPath(
                candidates,
                seen,
                (maybe_path / "genie_config.json").string());
        }
    }

    for (const auto& root : App::ModelCatalog::TextModelRoots()) {
        AddUniqueNormalizedPath(
            candidates,
            seen,
            (std::filesystem::path(root) / model_key / "genie_config.json").string());
    }

    return candidates;
}

}  // namespace

namespace App::ModelCatalog {
std::string ModelIdFromConfigJson(const std::string& config_json,
                                  const std::string& fallback) {
    try {
        const auto cfg = json::parse(config_json);
        if (cfg.contains("model_id") && cfg["model_id"].is_string()) {
            return cfg["model_id"].get<std::string>();
        }
        if (cfg.contains("model") && cfg["model"].is_string()) {
            return cfg["model"].get<std::string>();
        }
    } catch (...) {
    }

    return fallback;
}

std::string AliasModelIdForConfigPath(const std::string& config_path,
                                      const std::string& parsed_model_id) {
    // Extract folder name as model ID (no hardcoded aliasing)
    const std::filesystem::path config_file(config_path);
    const std::string folder_name = config_file.parent_path().filename().string();
    
    // Use folder name as model ID, fallback to parsed_model_id if empty
    if (!folder_name.empty()) {
        return folder_name;
    }

    const std::string model_id = parsed_model_id.empty() ? "genie" : parsed_model_id;
    return model_id;
}

std::string InferModelFamily(const std::string& config_path,
                             const std::string& model_id) {
    const std::string norm_model_id = ToLowerAscii(model_id);

    if (norm_model_id.find("qwen") != std::string::npos) {
        return "qwen";
    }
    if (norm_model_id.find("falcon") != std::string::npos) {
        return "falcon";
    }
    if (norm_model_id.find("llama") != std::string::npos) {
        return "llama";
    }
    // Default to llama for unknown model families
    return "llama";
}

std::string DefaultPromptTemplateForModel(const std::string& config_path,
                                          const std::string& model_id) {
    const std::string family = InferModelFamily(config_path, model_id);
    if (family == "qwen") {
        return "qwen";
    }
    if (family == "falcon") {
        return "falcon";
    }
    return "llama3";
}

std::vector<std::string> TextModelRoots() {
    std::vector<std::string> roots;
    std::set<std::string> seen;

    auto add_root = [&roots, &seen](const std::string& value) {
        AddUniqueNormalizedPath(roots, seen, value);
    };

    if (const char* env = std::getenv("TG_MODELS_ROOT")) {
        add_root(env);
    }
    if (const char* env = std::getenv("TEXT_GEN_MODELS_ROOT")) {
        add_root(env);
    }

    // Canonical repository model root used by compose/runtime.
    add_root("/opt/genai-studio-models/text-to-text");

    return roots;
}

std::vector<LocalModelEntry> DiscoverLocalModels(const std::string& active_config_path,
                                                 const std::string& active_model_id) {
    std::vector<LocalModelEntry> models;
    std::set<std::string> seen_configs;

    const std::string active_cfg_norm = ServerUtils::NormalizePath(active_config_path);

    for (const auto& root : TextModelRoots()) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) ||
            !std::filesystem::is_directory(root, ec)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;

            const std::filesystem::path config_path = entry.path() / "genie_config.json";
            const std::string config_norm = ServerUtils::NormalizePath(config_path.string());

            if (!IsRegularFilePath(config_norm)) continue;
            if (!seen_configs.insert(config_norm).second) continue;

            models.push_back(BuildLocalModelEntry(config_norm,
                                                  active_cfg_norm,
                                                  active_model_id,
                                                  false,
                                                  false));
        }
    }

    // Ensure active config is always visible even when it is outside TG_MODELS_ROOT
    // (for example /opt/genie_bundle/genie_config.json).
    if (!active_cfg_norm.empty() && IsRegularFilePath(active_cfg_norm) &&
        seen_configs.insert(active_cfg_norm).second) {
        models.push_back(BuildLocalModelEntry(active_cfg_norm,
                                              active_cfg_norm,
                                              active_model_id,
                                              true,
                                              true));
    }

    std::sort(models.begin(),
              models.end(),
              [](const LocalModelEntry& lhs, const LocalModelEntry& rhs) {
                  return lhs.key < rhs.key;
              });

    return models;
}

bool ResolveModelConfigPath(const json& body,
                            std::string& resolved_config_path,
                            std::string& resolved_model_key,
                            std::string& err) {
    const std::vector<std::string> candidates =
        BuildCandidateConfigPaths(body, resolved_model_key);

    for (const auto& candidate : candidates) {
        if (!IsRegularFilePath(candidate)) continue;

        resolved_config_path = ServerUtils::NormalizePath(candidate);
        if (resolved_model_key.empty()) {
            resolved_model_key =
                std::filesystem::path(resolved_config_path).parent_path().filename().string();
        }
        return true;
    }

    err = "Could not resolve model config. Provide one of: "
          "genie_config_path, model_dir, or model.";
    return false;
}

bool NormalizeRequestedModelName(const std::string& requested_model,
                                 const std::string& active_model_id,
                                 std::string& normalized_model) {
    // If requested model matches active model ID exactly, accept it
    if (requested_model == active_model_id) {
        normalized_model = active_model_id;
        return true;
    }

    // Special case: "genie" is an orchestrator alias for the active model
    if (requested_model == "genie") {
        normalized_model = active_model_id;
        return true;
    }

    // Normalize both to lowercase for comparison
    const std::string requested_lower = ToLowerAscii(requested_model);
    const std::string active_lower = ToLowerAscii(active_model_id);

    // Extract model family from requested model name
    std::string requested_family;
    if (requested_lower.find("llama") != std::string::npos) {
        requested_family = "llama";
    } else if (requested_lower.find("qwen") != std::string::npos) {
        requested_family = "qwen";
    } else if (requested_lower.find("falcon") != std::string::npos) {
        requested_family = "falcon";
    }

    // Extract model family from active model ID
    std::string active_family;
    if (active_lower.find("llama") != std::string::npos) {
        active_family = "llama";
    } else if (active_lower.find("qwen") != std::string::npos) {
        active_family = "qwen";
    } else if (active_lower.find("falcon") != std::string::npos) {
        active_family = "falcon";
    }

    // If both belong to the same model family, accept the requested model
    // and map it to the active model ID
    if (!requested_family.empty() && requested_family == active_family) {
        normalized_model = active_model_id;
        return true;
    }

    // No match found
    normalized_model = requested_model;
    return false;
}
}  // namespace App::ModelCatalog
