// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "AsrService.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void AsrService::handleOpenAiTranscriptions(const httplib::Request& req,
                                            httplib::Response& res) {
    handleOpenAiRequest(
        req,
        res,
        OpenAiAudioRouteKind::Transcriptions,
        /*translate=*/false,
        "transcribe",
        counters_.openai_transcriptions_total);
}

void AsrService::handleOpenAiTranslations(const httplib::Request& req,
                                          httplib::Response& res) {
    handleOpenAiRequest(
        req,
        res,
        OpenAiAudioRouteKind::Translations,
        /*translate=*/true,
        "translate",
        counters_.openai_translations_total);
}

void AsrService::handleOpenAiTranscriptionStream(const httplib::Request& req,
                                                 httplib::Response& res) {
    handleOpenAiRequest(
        req,
        res,
        OpenAiAudioRouteKind::TranscriptionsStream,
        /*translate=*/false,
        "transcribe",
        counters_.openai_stream_total);
}

void AsrService::registerOpenAiRoutes(httplib::Server& svr) {
    svr.Post("/v1/audio/transcriptions",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleOpenAiTranscriptions(req, res);
    });

    svr.Post("/v1/audio/translations",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleOpenAiTranslations(req, res);
    });

    svr.Get("/v1/audio/transcriptions/stream",
    [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{
            {"endpoint",     "/v1/audio/transcriptions/stream"},
            {"method",       "POST"},
            {"description",  "Stream transcription as SSE. "
                             "Send multipart/form-data with 'file' field. "
                             "Response: text/event-stream with "
                             "transcript.text.delta and transcript.text.done events."},
            {"fields",       json::array({"file (required)", "model (required)",
                                          "language (optional)",
                                          "response_format (ignored for stream)"})}
        }.dump(), "application/json");
    });

    svr.Post("/v1/audio/transcriptions/stream",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleOpenAiTranscriptionStream(req, res);
    });
}
