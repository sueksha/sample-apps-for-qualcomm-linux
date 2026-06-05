// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
//
// TtsApp – C++ HTTP service wrapping the melo_sdk TTSEngine.
//
// The melo_sdk (libtts.so) loads a single bundled model file whose
// binary layout is produced by qnn_model_generation.py:
//
//   [Header 16B]  magic '0LEM' + version + total_size
//   [melo_struct] config + all sub-model sizes + working buffers (256B-aligned)
//   [dict_data]   word/phone/tone dictionaries (256B-aligned)
//   [bert.bin]    BERT model (256B-aligned, optional for non-English)
//   [encoder.bin] VITS encoder (256B-aligned)
//   [flow.bin]    normalizing flow (256B-aligned)
//   [decoder.bin] HiFi-GAN decoder (256B-aligned)
//   [g2p_enc.bin] G2P encoder (256B-aligned, optional)
//   [g2p_dec.bin] G2P decoder (256B-aligned, optional)
//   [tokenizer]   BERT tokenizer binary (256B-aligned)
//   [normalizer]  Unicode normalizer binary (256B-aligned)
//   [scratch_mem] Pre-allocated scratch memory (256B-aligned)
//   [Footer 8B]   magic 'MEL0MODE'
//
// The SDK handles all text preprocessing (G2P, BERT, tokenization),
// VITS inference (encoder → flow → decoder), and returns PCM audio
// via the TTSResultCallback interface.
// ---------------------------------------------------------------------

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "TTSConfig.h"
#include "TTSEngine.h"
#include "TTSError.h"
#include "TTSResultCallback.h"

// Forward declarations
namespace httplib {
class Server;
class Request;
class Response;
}

namespace TtsService {

// ===========================================================================
// WAV header (44 bytes, PCM format)
// ===========================================================================
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]         = {'R', 'I', 'F', 'F'};
    uint32_t chunk_size      = 0;
    char     wave[4]         = {'W', 'A', 'V', 'E'};
    char     fmt[4]          = {'f', 'm', 't', ' '};
    uint32_t subchunk1_size  = 16;
    uint16_t audio_format    = 1;      // PCM = 1
    uint16_t num_channels    = 1;
    uint32_t sample_rate     = 44100;
    uint32_t byte_rate       = 0;
    uint16_t block_align     = 0;
    uint16_t bits_per_sample = 16;
    char     data[4]         = {'d', 'a', 't', 'a'};
    uint32_t data_size       = 0;
};
#pragma pack(pop)

// ===========================================================================
// AudioCollector – TTSResultCallback implementation
//
// Collects PCM chunks from the SDK callbacks into a contiguous buffer,
// then wraps them in a WAV container.
// ===========================================================================
class AudioCollector : public TTS::TTSResultCallback {
public:
    // Audio format (set by onStart)
    int32_t sample_rate     = 44100;
    int32_t bits_per_sample = 16;
    int32_t num_channels    = 1;

    // Collected PCM data
    std::vector<uint8_t> pcm_data;

    // Status
    bool        synthesis_done = false;
    bool        has_error      = false;
    std::string error_msg;

    // Reset state before each synthesis call
    void reset() {
        pcm_data.clear();
        synthesis_done = false;
        has_error      = false;
        error_msg.clear();
    }

    // ---- TTSResultCallback interface ----------------------------------------

    // Called once at the start of synthesis with audio format info
    void onStart(int32_t sr, int32_t bps, int32_t nc) override {
        sample_rate     = sr;
        bits_per_sample = bps;
        num_channels    = nc;
    }

    // Called multiple times with PCM audio chunks
    // buff: raw PCM bytes, offset: start index, size: valid bytes
    void onAudioAvailable(std::vector<uint8_t> buff,
                          int32_t              offset,
                          int32_t              size) override {
        if (offset >= 0 &&
            size > 0 &&
            (offset + size) <= static_cast<int32_t>(buff.size())) {
            pcm_data.insert(pcm_data.end(),
                            buff.begin() + offset,
                            buff.begin() + offset + size);
        }
    }

    // Called once when synthesis is complete
    void onDone() override {
        synthesis_done = true;
    }

    // Called on synthesis error
    void onError(TTS::TTSError error) override {
        has_error = true;
        switch (error) {
            case TTS::TTSError::ERROR_SYNTHESIS:
                error_msg = "TTS synthesis error";
                break;
            case TTS::TTSError::ERROR_INVALID_REQUEST:
                error_msg = "TTS invalid request";
                break;
            default:
                error_msg = "TTS unknown error";
                break;
        }
    }

    // Build a complete WAV file from the collected PCM data
    std::vector<uint8_t> toWav() const;

    // Duration of collected audio in milliseconds
    double durationMs() const {
        if (sample_rate == 0 || num_channels == 0 || bits_per_sample == 0)
            return 0.0;
        const double bytes_per_sec =
            static_cast<double>(sample_rate) * num_channels * (bits_per_sample / 8);
        return (pcm_data.size() / bytes_per_sec) * 1000.0;
    }
};

// ===========================================================================
// TtsApp – HTTP service wrapping TTSEngine
// ===========================================================================
class TtsApp {
public:
    TtsApp(const std::string& model_path,
           const std::string& language,
           float              speaking_rate,
           float              pitch,
           float              volume_gain,
           int                sample_rate,
           int                port);

    ~TtsApp();

    // Start the HTTP server (blocks until server exits)
    void run();

private:
    // Configuration
    std::string model_path_;
    std::string language_;
    float       speaking_rate_;
    float       pitch_;
    float       volume_gain_;
    int         sample_rate_;
    int         port_;

    // melo_sdk objects
    TTS::TTSEngine* engine_   = nullptr;
    AudioCollector* callback_ = nullptr;

    // Serialise TTS requests (one at a time)
    std::timed_mutex tts_mutex_;

    // Timing
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<int64_t>                  server_created_unix_{0};

    // ---- Internal helpers --------------------------------------------------

    // Resolve model file path:
    //   - If model_path_ is a regular file, use it directly.
    //   - If model_path_ is a directory, search for the first *.qnn file.
    std::string resolveModelFile() const;

    // Run one TTS synthesis and return WAV bytes.
    // Caller must hold tts_mutex_.
    std::vector<uint8_t> synthesize(const std::string& text);

    // Register all HTTP routes on svr
    void setupRoutes(httplib::Server& svr);
    void setupErrorHandlers(httplib::Server& svr);
    void registerHealthRoute(httplib::Server& svr);
    void registerModelsRoute(httplib::Server& svr);
    void registerMethodNotAllowedRoutes(httplib::Server& svr);
    void registerGenerateRoute(httplib::Server& svr);
    void registerOpenAiSpeechRoute(httplib::Server& svr);
    void handleSynthesisRequest(const httplib::Request& req,
                                httplib::Response&      res,
                                const char*             text_field,
                                const char*             missing_field_error,
                                bool                    require_openai_contract = false);
    void handleGenerateRequest(const httplib::Request& req,
                               httplib::Response&      res);
    void handleOpenAiSpeechRequest(const httplib::Request& req,
                                   httplib::Response&      res);
};

} // namespace TtsService
