// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>

#include "server/HttpServer.hpp"

namespace App::ModelAccessPolicy {
inline constexpr std::chrono::seconds kBusyWaitTimeout{30};
inline constexpr const char* kBusyRetryAfterSeconds = "2";

class DrainFlagGuard final {
  public:
    explicit DrainFlagGuard(std::atomic<bool>& drain_flag) noexcept : flag_(drain_flag) {
        flag_.store(true, std::memory_order_relaxed);
    }
    ~DrainFlagGuard() { flag_.store(false, std::memory_order_relaxed); }

    DrainFlagGuard(const DrainFlagGuard&) = delete;
    DrainFlagGuard& operator=(const DrainFlagGuard&) = delete;
    DrainFlagGuard(DrainFlagGuard&&) = delete;
    DrainFlagGuard& operator=(DrainFlagGuard&&) = delete;

  private:
    std::atomic<bool>& flag_;
};

inline void SetRetryAfterHeader(httplib::Response& res) {
    res.set_header("Retry-After", kBusyRetryAfterSeconds);
}
} // namespace App::ModelAccessPolicy
