// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <chrono>
#include "Whisper.h"
#include "InputStream.h"
#include "WhisperResponseListener.h"

// ---------------------------------------------------------------------------
// Result returned by every transcription call
// ---------------------------------------------------------------------------
struct TranscriptionTiming {
    uint64_t configure_ms = 0;
    uint64_t start_ms     = 0;
    uint64_t feed_ms      = 0;
    uint64_t wait_ms      = 0;
    uint64_t stop_ms      = 0;
    uint64_t total_ms     = 0;
};

struct TranscriptionResult {
    std::string text;
    std::string language;
    bool        success         = false;
    std::string error;
    uint64_t    process_time_ms = 0;
    TranscriptionTiming timing;
};

// ---------------------------------------------------------------------------
// WhisperEngine
//
// Thread-safe, synchronous wrapper around the async Whisper SDK.
// Only one transcription request runs at a time (serialised by request_mu_).
// ---------------------------------------------------------------------------
class WhisperEngine {
public:
    explicit WhisperEngine(const std::string& model_path,
                           const std::string& vad_model_path = "");
    ~WhisperEngine();

    WhisperEngine(const WhisperEngine&)            = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // Load the model.  Throws std::runtime_error on failure.
    void initialize();

    // Unload the model (safe to call even if not initialised).
    void shutdown() noexcept;

    bool        isReady()    const noexcept;
    std::string getVersion();   // non-const: Whisper::getVersion() is not const

    // Transcribe a WAV file (path on disk).
    // Set translate=true to translate the audio to English.
    // Blocks until the final transcription arrives or timeout expires.
    TranscriptionResult transcribeFile(
        const std::string& wav_path,
        const std::string& language_code = "en",
        bool               translate     = false,
        std::chrono::seconds timeout     = std::chrono::seconds(120));

    // Transcribe raw PCM audio (16 kHz, 16-bit, mono, little-endian).
    TranscriptionResult transcribePCM(
        const std::vector<uint8_t>& pcm_data,
        const std::string& language_code = "en",
        bool               translate     = false,
        std::chrono::seconds timeout     = std::chrono::seconds(120));

private:
    // Internal listener – bridges async SDK callbacks to a blocking wait
    class TranscriptionListener;

    std::string model_path_;
    std::string vad_model_path_;
    WhisperFunction::Whisper whisper_;
    std::unique_ptr<TranscriptionListener> listener_;

    // Serialises concurrent HTTP requests (Whisper SDK is not thread-safe)
    mutable std::mutex request_mu_;
    bool initialized_ = false;

    TranscriptionResult doTranscribe(
        bool                        use_file,
        const std::string&          wav_path,
        const std::vector<uint8_t>& pcm_data,
        const std::string&          language_code,
        bool                        translate,
        std::chrono::seconds        timeout);
};
