// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "Genie.hpp"
#include "server/HttpServer.hpp"

namespace App::CompletionService {
void HandleChatCompletionPost(Genie& genie,
                              std::timed_mutex& model_mutex,
                              std::mutex& stream_worker_mutex,
                              size_t& active_stream_worker_count,
                              std::condition_variable& stream_workers_cv,
                              const std::string& active_model_id,
                              const std::string& active_model_config_path,
                              uint32_t max_tokens_default,
                              bool allow_debug_template_override,
                              const httplib::Request& req,
                              httplib::Response& res);
} // namespace App::CompletionService
