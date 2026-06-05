// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
namespace AppUtils
{

class PromptHandler
{
  private:
    std::string m_system_prompt{"You're a helpful AI assistant"};
    std::string m_prompt_template{"llama3"};

  public:
    PromptHandler() = default;
    void SetSystemPrompt(std::string system_prompt);
    std::string GetPromptWithTag(const std::string& user_prompt);
};

} // namespace AppUtils
