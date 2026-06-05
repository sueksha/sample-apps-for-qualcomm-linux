// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "server/ServerUtils.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using json = nlohmann::json;

namespace App::ServerUtils {
std::string SafeStr(const json& j, const std::string& key, const std::string& def) {
    try {
        if (!j.contains(key) || j[key].is_null()) return def;
        const auto& v = j[key];
        if (v.is_string()) return v.get<std::string>();
        return v.dump();   // coerce number/bool to its JSON text
    } catch (...) { return def; }
}

bool SafeBool(const json& j, const std::string& key, bool def) {
    try {
        if (!j.contains(key) || j[key].is_null()) return def;
        const auto& v = j[key];
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_number())  return v.get<double>() != 0.0;
        if (v.is_string())  {
            const auto s = v.get<std::string>();
            return s == "true" || s == "1";
        }
        return def;
    } catch (...) { return def; }
}

int SafeInt(const json& j, const std::string& key, int def) {
    try {
        if (!j.contains(key) || j[key].is_null()) return def;
        const auto& v = j[key];
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_number_float())   return static_cast<int>(v.get<double>());
        if (v.is_string()) {
            try { return std::stoi(v.get<std::string>()); } catch (...) {}
        }
        return def;
    } catch (...) { return def; }
}

std::string NormalizePath(const std::string& path) {
    try {
        return std::filesystem::weakly_canonical(std::filesystem::path(path)).string();
    } catch (...) {
        return path;
    }
}

bool ReadFileToString(const std::string& file_path, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    std::ifstream file(file_path);
    if (!file) {
        err = "Failed to open file: " + file_path;
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (out.empty()) {
        err = "File is empty: " + file_path;
        return false;
    }
    return true;
}

std::string SanitizeContent(std::string s) {
    // Remove null bytes that would truncate C-string handling in Genie.
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    return s;
}

int64_t UnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void SetJsonError(httplib::Response& res,
                  int status,
                  const std::string& message,
                  const std::string& type,
                  const std::string& code,
                  const std::string& param) {
    const std::string normalized_code = code.empty() ? type : code;
    json err{{"error",
              {{"message", message},
               {"type", type},
               {"code", normalized_code.empty() ? json(nullptr) : json(normalized_code)},
               {"param", param.empty() ? json(nullptr) : json(param)}}}};

    res.status = status;
    res.set_content(err.dump(), "application/json");
}
} // namespace App::ServerUtils
