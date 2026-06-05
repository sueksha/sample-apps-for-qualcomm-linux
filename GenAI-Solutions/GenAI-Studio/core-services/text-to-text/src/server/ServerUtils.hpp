// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "server/HttpServer.hpp"

namespace App::ServerUtils {
std::string SafeStr(const nlohmann::json& j, const std::string& key,
                    const std::string& def = "");
bool SafeBool(const nlohmann::json& j, const std::string& key, bool def = false);
int SafeInt(const nlohmann::json& j, const std::string& key, int def = -1);
std::string NormalizePath(const std::string& path);
bool ReadFileToString(const std::string& file_path, std::string& out, std::string& err);
std::string SanitizeContent(std::string s);
int64_t UnixTimestamp();
void SetJsonError(httplib::Response& res,
                  int status,
                  const std::string& message,
                  const std::string& type = "invalid_request_error",
                  const std::string& code = "",
                  const std::string& param = "");
} // namespace App::ServerUtils
