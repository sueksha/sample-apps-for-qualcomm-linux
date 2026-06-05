// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
// DEPRECATED PATH: kept for reference. Current tts-service runtime is
// built from main.cpp + TtsApp.cpp (see CMakeLists.txt).
#include "TtsService.hpp"

#include "crow.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// Map OpenAI voice name → TTS SDK language string
static std::string voiceToLanguage(const std::string& voice) {
    const std::string v = toLower(voice);
    if (v == "chinese" || v == "zh" || v == "mandarin") return "Chinese";
    if (v == "spanish" || v == "es")                    return "Spanish";
    // alloy, echo, fable, onyx, nova, shimmer → English
    return "English";
}

// Map OpenAI response_format → AudioEncoding + Content-Type + add_wav_header
struct FormatInfo {
    AudioEncoding encoding;
    std::string   content_type;
    bool          add_wav_header;
};

static FormatInfo resolveFormat(const std::string& fmt) {
    const std::string f = toLower(fmt);
    if (f == "mp3")  return {AudioEncoding::MP3,      "audio/mpeg",       false};
    if (f == "opus") return {AudioEncoding::OGG_OPUS,  "audio/ogg",        false};
    if (f == "wav")  return {AudioEncoding::LINEAR16,  "audio/wav",        true };
    if (f == "pcm")  return {AudioEncoding::LINEAR16,  "audio/pcm",        false};
    if (f == "aac")  return {AudioEncoding::LINEAR16,  "audio/wav",        true }; // fallback
    if (f == "flac") return {AudioEncoding::LINEAR16,  "audio/wav",        true }; // fallback
    // default: mp3
    return {AudioEncoding::MP3, "audio/mpeg", false};
}

static json errorJson(const std::string& msg, const std::string& type = "server_error") {
    return json{{"error", {{"message", msg}, {"type", type}}}};
}

static void registerOpenAiSpeechRoute(crow::SimpleApp& app, TtsEngine& engine) {
    // ======================================================================
    // POST /v1/audio/speech   (OpenAI TTS API)
    // ======================================================================
    CROW_ROUTE(app, "/v1/audio/speech").methods(crow::HTTPMethod::Post)
    ([&engine](const crow::request& req, crow::response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception& e) {
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson(std::string("Invalid JSON: ") + e.what(),
                                "invalid_request_error").dump());
            res.end();
            return;
        }

        if (!body.contains("input") || !body["input"].is_string() ||
            body["input"].get<std::string>().empty()) {
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson("'input' field is required and must be a non-empty string",
                                "invalid_request_error").dump());
            res.end();
            return;
        }

        const std::string input  = body["input"].get<std::string>();
        const std::string voice  = body.value("voice",           "alloy");
        const std::string fmt    = body.value("response_format", "mp3");
        const float       speed  = body.value("speed",           1.0f);

        const std::string language = voiceToLanguage(voice);
        const FormatInfo  fi       = resolveFormat(fmt);
        const float clamped_speed  = std::max(0.25f, std::min(4.0f, speed));

        std::cout << "[TTS] POST /v1/audio/speech"
                  << " input_len=" << input.size()
                  << " voice=" << voice
                  << " lang=" << language
                  << " fmt=" << fmt
                  << " speed=" << clamped_speed << "\n";

        SynthesisRequest sr;
        sr.text           = input;
        sr.language       = language;
        sr.encoding       = fi.encoding;
        sr.speed          = clamped_speed;
        sr.add_wav_header = fi.add_wav_header;

        const SynthesisResult result = engine.synthesize(sr);
        if (!result.success) {
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson(result.error).dump());
            res.end();
            return;
        }

        std::cout << "[TTS] Returning " << result.audio.size()
                  << " bytes (" << fi.content_type << ") in "
                  << result.process_time_ms << " ms\n";

        res.set_header("Content-Type", fi.content_type);
        res.set_header("X-Process-Time-Ms",
                       std::to_string(result.process_time_ms));
        res.write(std::string(result.audio.begin(), result.audio.end()));
        res.end();
    });
}

static void registerLegacyTtsRoute(crow::SimpleApp& app, TtsEngine& engine) {
    // ======================================================================
    // POST /tts   (simple endpoint)
    // ======================================================================
    CROW_ROUTE(app, "/tts").methods(crow::HTTPMethod::Post)
    ([&engine](const crow::request& req, crow::response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson("Invalid JSON body", "invalid_request_error").dump());
            res.end();
            return;
        }

        if (!body.contains("text") || !body["text"].is_string() ||
            body["text"].get<std::string>().empty()) {
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson("'text' field is required", "invalid_request_error").dump());
            res.end();
            return;
        }

        const std::string text     = body["text"].get<std::string>();
        const std::string language = body.value("language",    "English");
        const std::string fmt      = body.value("format",      "wav");
        const float       speed    = body.value("speed",       1.0f);
        const float       pitch    = body.value("pitch",       0.0f);
        const float       volume   = body.value("volume",      0.0f);
        const int32_t     sr       = body.value("sample_rate", 44100);

        const FormatInfo fi = resolveFormat(fmt);

        std::cout << "[TTS] POST /tts text_len=" << text.size()
                  << " lang=" << language << " fmt=" << fmt << "\n";

        SynthesisRequest sreq;
        sreq.text           = text;
        sreq.language       = language;
        sreq.encoding       = fi.encoding;
        sreq.speed          = std::max(0.25f, std::min(4.0f, speed));
        sreq.pitch          = std::max(-20.0f, std::min(20.0f, pitch));
        sreq.volume_gain    = std::max(-96.0f, std::min(16.0f, volume));
        sreq.sample_rate    = sr;
        sreq.add_wav_header = fi.add_wav_header;

        const SynthesisResult result = engine.synthesize(sreq);
        if (!result.success) {
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.write(errorJson(result.error).dump());
            res.end();
            return;
        }

        res.set_header("Content-Type", fi.content_type);
        res.set_header("X-Process-Time-Ms",
                       std::to_string(result.process_time_ms));
        res.write(std::string(result.audio.begin(), result.audio.end()));
        res.end();
    });
}

static void registerHealthRoute(crow::SimpleApp& app, TtsEngine& engine) {
    // ======================================================================
    // GET /health
    // ======================================================================
    CROW_ROUTE(app, "/health").methods(crow::HTTPMethod::Get)
    ([&engine](const crow::request&, crow::response& res) {
        const bool ready = engine.isReady();
        res.code = ready ? 200 : 503;
        res.set_header("Content-Type", "application/json");
        res.write(json{{"status", ready ? "ok" : "not_ready"}}.dump());
        res.end();
    });
}

static void registerModelsRoute(crow::SimpleApp& app) {
    // ======================================================================
    // GET /v1/models   (OpenAI models list)
    // ======================================================================
    CROW_ROUTE(app, "/v1/models").methods(crow::HTTPMethod::Get)
    ([](const crow::request&, crow::response& res) {
        res.set_header("Content-Type", "application/json");
        res.write(json{
            {"object", "list"},
            {"data", json::array({
                json{{"id","tts-1"},    {"object","model"}, {"owned_by","qualcomm"}},
                json{{"id","tts-1-hd"}, {"object","model"}, {"owned_by","qualcomm"}}
            })}
        }.dump());
        res.end();
    });
}

static void registerVoicesRoute(crow::SimpleApp& app) {
    // ======================================================================
    // GET /voices   – list supported voices / languages
    // ======================================================================
    CROW_ROUTE(app, "/voices").methods(crow::HTTPMethod::Get)
    ([](const crow::request&, crow::response& res) {
        res.set_header("Content-Type", "application/json");
        res.write(json{
            {"voices", json::array({
                json{{"id","alloy"},   {"language","English"}, {"description","Default English voice"}},
                json{{"id","echo"},    {"language","English"}, {"description","English voice"}},
                json{{"id","fable"},   {"language","English"}, {"description","English voice"}},
                json{{"id","onyx"},    {"language","English"}, {"description","English voice"}},
                json{{"id","nova"},    {"language","English"}, {"description","English voice"}},
                json{{"id","shimmer"}, {"language","English"}, {"description","English voice"}},
                json{{"id","chinese"}, {"language","Chinese"}, {"description","Mandarin Chinese voice"}},
                json{{"id","spanish"}, {"language","Spanish"}, {"description","Spanish voice"}}
            })},
            {"formats", json::array({"mp3","opus","wav","pcm"})},
            {"speed_range", json{{"min",0.25},{"max",4.0},{"default",1.0}}}
        }.dump());
        res.end();
    });
}

// ---------------------------------------------------------------------------
// TtsService
// ---------------------------------------------------------------------------

TtsService::TtsService(const std::string& model_path, uint16_t port)
    : engine_(model_path), port_(port)
{
    engine_.initialize();
}

void TtsService::run() {
    crow::SimpleApp app;
    registerOpenAiSpeechRoute(app, engine_);
    registerLegacyTtsRoute(app, engine_);
    registerHealthRoute(app, engine_);
    registerModelsRoute(app);
    registerVoicesRoute(app);

    std::cout << "[TtsService] Listening on 0.0.0.0:" << port_ << "\n";
    app.bindaddr("0.0.0.0").port(port_).multithreaded().run();
}
