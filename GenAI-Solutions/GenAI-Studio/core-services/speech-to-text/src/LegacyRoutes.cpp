// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "AsrService.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"

void AsrService::registerLegacyRoutes(httplib::Server& svr) {
    svr.Post("/generate",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleLegacyMultipartTranscription(req, res);
    });
    svr.Post("/transcribe",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleLegacyMultipartTranscription(req, res);
    });

    svr.Post("/transcribe/raw",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleLegacyRawTranscription(req, res);
    });
}

