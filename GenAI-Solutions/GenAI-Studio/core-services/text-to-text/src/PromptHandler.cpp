// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "PromptHandler.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "server/CompletionRuntime.hpp"

using namespace AppUtils;
using json = nlohmann::json;

namespace {
std::string SanitizePromptText(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());
    return value;
}
} // namespace

void PromptHandler::SetSystemPrompt(std::string system_prompt) {
    m_system_prompt = SanitizePromptText(std::move(system_prompt));
}

std::string PromptHandler::GetPromptWithTag(const std::string& user_prompt) {
    const std::string sanitized_user_prompt = SanitizePromptText(user_prompt);

    json messages = json::array();
    if (!m_system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", m_system_prompt}});
    }
    messages.push_back({{"role", "user"}, {"content", sanitized_user_prompt}});

    bool has_user_content = false;
    std::string parse_error;
    const std::string prompt = App::CompletionRuntime::BuildPromptFromMessages(
        messages, has_user_content, parse_error, m_prompt_template);

    if (!parse_error.empty()) {
        throw std::runtime_error("Prompt build failed: " + parse_error);
    }
    if (!has_user_content) {
        throw std::runtime_error("Prompt build failed: missing user content");
    }
    return prompt;
}
