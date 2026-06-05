// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
// DEPRECATED PATH: kept for reference. Current tts-service runtime is
// built from main.cpp + TtsApp.cpp (see CMakeLists.txt).
#pragma once

#include <string>
#include <cstdint>
#include "TtsEngine.hpp"

// ---------------------------------------------------------------------------
// TtsService
//
// Crow-based HTTP server exposing the MeloTTS engine.
//
// OpenAI-compatible endpoints:
//   POST /v1/audio/speech   – OpenAI TTS API
//   GET  /v1/models         – lists tts-1 and tts-1-hd
//
// Additional endpoints:
//   POST /tts               – simple JSON body {"text":"..."}
//   GET  /health            – liveness check
//
// OpenAI /v1/audio/speech request body (JSON):
//   {
//     "model":           "tts-1",          // accepted, ignored
//     "input":           "Hello world",    // required
//     "voice":           "alloy",          // alloy|echo|fable|onyx|nova|shimmer
//                                          // also: "chinese", "spanish"
//     "response_format": "mp3",            // mp3|opus|wav|pcm  (default mp3)
//     "speed":           1.0               // 0.25–4.0 (default 1.0)
//   }
//
// Response: binary audio with appropriate Content-Type header.
// ---------------------------------------------------------------------------
class TtsService {
public:
    TtsService(const std::string& model_path, uint16_t port);
    void run();

private:
    TtsEngine engine_;
    uint16_t  port_;
};
