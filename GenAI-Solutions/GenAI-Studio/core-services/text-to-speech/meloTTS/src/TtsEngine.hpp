// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
// DEPRECATED PATH: kept for reference. Current tts-service runtime is
// built from main.cpp + TtsApp.cpp (see CMakeLists.txt).
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <chrono>
#include "TTSEngine.h"
#include "TTSResultCallback.h"
#include "TTSConfig.h"
#include "TTSError.h"

// ---------------------------------------------------------------------------
// Audio encoding codes (mirrors TTS::AUDIO_ENCODING_FORMAT)
// ---------------------------------------------------------------------------
enum class AudioEncoding : int {
    LINEAR16 = 0,
    MP3      = 1,
    OGG_OPUS = 2,
    MULAW    = 3,
    ALAW     = 4
};

// ---------------------------------------------------------------------------
// Per-request synthesis parameters
// ---------------------------------------------------------------------------
struct SynthesisRequest {
    std::string   text;
    std::string   language      = "English";  // English | Chinese | Spanish
    AudioEncoding encoding      = AudioEncoding::LINEAR16;
    float         speed         = 1.0f;   // 0.25 – 4.0
    float         pitch         = 0.0f;   // -20.0 – 20.0
    float         volume_gain   = 0.0f;   // -96.0 – 16.0
    int32_t       sample_rate   = 44100;
    bool          add_wav_header = false; // prepend RIFF/WAV header to LINEAR16 output
};

// ---------------------------------------------------------------------------
// Result of a synthesis call
// ---------------------------------------------------------------------------
struct SynthesisResult {
    std::vector<uint8_t> audio;           // raw audio bytes
    int32_t  sample_rate     = 44100;
    int32_t  bits_per_sample = 16;
    int32_t  channels        = 1;
    bool     success         = false;
    std::string error;
    uint64_t process_time_ms = 0;
};

// ---------------------------------------------------------------------------
// TtsEngine
//
// Thread-safe wrapper around the TTS::TTSEngine singleton.
// Requests are serialised (one synthesis at a time).
// The engine is re-initialised automatically when the config changes.
// ---------------------------------------------------------------------------
class TtsEngine {
public:
    explicit TtsEngine(const std::string& model_path);
    ~TtsEngine();

    TtsEngine(const TtsEngine&)            = delete;
    TtsEngine& operator=(const TtsEngine&) = delete;

    // Load the model with default config (English, LINEAR16, 44100 Hz).
    // Throws std::runtime_error on failure.
    void initialize();

    // Unload the model (safe to call even if not initialised).
    void shutdown() noexcept;

    bool isReady() const noexcept;

    // Synthesise text → audio bytes.
    // Blocks until synthesis is complete.
    SynthesisResult synthesize(const SynthesisRequest& req);

    // Build a 44-byte RIFF/WAV header for the given PCM parameters.
    static std::vector<uint8_t> buildWavHeader(int32_t sample_rate,
                                               int32_t bits_per_sample,
                                               int32_t channels,
                                               int32_t data_size);

private:
    // Internal callback that collects audio chunks synchronously
    class AudioCollector;

    // Tracks the currently loaded engine configuration
    struct EngineConfig {
        std::string   language    = "English";
        AudioEncoding encoding    = AudioEncoding::LINEAR16;
        float         speed       = 1.0f;
        float         pitch       = 0.0f;
        float         volume_gain = 0.0f;
        int32_t       sample_rate = 44100;

        bool operator==(const EngineConfig& o) const noexcept {
            return language    == o.language    &&
                   encoding    == o.encoding    &&
                   speed       == o.speed       &&
                   pitch       == o.pitch       &&
                   volume_gain == o.volume_gain &&
                   sample_rate == o.sample_rate;
        }
        bool operator!=(const EngineConfig& o) const noexcept {
            return !(*this == o);
        }
    };

    std::string model_path_;
    TTS::TTSEngine* engine_ = nullptr;
    std::unique_ptr<AudioCollector> collector_;
    mutable std::mutex mu_;
    bool initialized_ = false;
    EngineConfig current_config_;

    // (Re-)initialise the SDK with the given config (must hold mu_)
    void doInit(const EngineConfig& cfg);
    void doShutdown() noexcept;
};
