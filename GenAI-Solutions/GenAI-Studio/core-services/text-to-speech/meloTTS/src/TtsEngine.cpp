// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
// DEPRECATED PATH: kept for reference. Current tts-service runtime is
// built from main.cpp + TtsApp.cpp (see CMakeLists.txt).
#include "TtsEngine.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <chrono>

// ==========================================================================
// AudioCollector
//
// Synchronous callback: TTSEngine::start() is blocking, so all callbacks
// fire on the calling thread before start() returns.
// We simply accumulate the audio chunks in a vector.
// ==========================================================================
class TtsEngine::AudioCollector : public TTS::TTSResultCallback
{
public:
    void reset() {
        audio_.clear();
        sample_rate_     = 44100;
        bits_per_sample_ = 16;
        channels_        = 1;
        error_           = false;
        error_type_      = TTS::ERROR_UNKNOWN;
    }

    // -----------------------------------------------------------------------
    // SDK callbacks
    // -----------------------------------------------------------------------

    // Called once at the start of synthesis with audio format info
    void onStart(int32_t sampleRateInHz,
                 int32_t audioFormat,
                 int32_t channelCount) override
    {
        sample_rate_     = sampleRateInHz;
        bits_per_sample_ = audioFormat;   // SDK passes bits-per-sample here
        channels_        = channelCount;
        std::cout << "[TtsEngine] onStart: " << sampleRateInHz << " Hz, "
                  << audioFormat << " bps, " << channelCount << " ch\n";
    }

    // Called repeatedly with audio chunks
    void onAudioAvailable(std::vector<uint8_t> buff,
                          int32_t offset,
                          int32_t size) override
    {
        if (offset < 0 || size <= 0 ||
            offset + size > static_cast<int32_t>(buff.size())) {
            std::cerr << "[TtsEngine] onAudioAvailable: invalid range "
                      << offset << "+" << size
                      << " in buf of " << buff.size() << "\n";
            return;
        }
        audio_.insert(audio_.end(),
                      buff.begin() + offset,
                      buff.begin() + offset + size);
    }

    // Called once when synthesis is complete
    void onDone() override {
        std::cout << "[TtsEngine] onDone: " << audio_.size() << " bytes\n";
    }

    // Called on synthesis error
    void onError(TTS::TTSError error) override {
        error_      = true;
        error_type_ = error;
        std::cerr << "[TtsEngine] onError: " << static_cast<int>(error) << "\n";
    }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    const std::vector<uint8_t>& audio()          const { return audio_; }
    int32_t                     sampleRate()      const { return sample_rate_; }
    int32_t                     bitsPerSample()   const { return bits_per_sample_; }
    int32_t                     channels()        const { return channels_; }
    bool                        hasError()        const { return error_; }
    TTS::TTSError               errorType()       const { return error_type_; }

    static std::string errorName(TTS::TTSError e) {
        switch (e) {
            case TTS::ERROR_SYNTHESIS:       return "ERROR_SYNTHESIS";
            case TTS::ERROR_INVALID_REQUEST: return "ERROR_INVALID_REQUEST";
            default:                         return "ERROR_UNKNOWN";
        }
    }

private:
    std::vector<uint8_t> audio_;
    int32_t              sample_rate_     = 44100;
    int32_t              bits_per_sample_ = 16;
    int32_t              channels_        = 1;
    bool                 error_           = false;
    TTS::TTSError        error_type_      = TTS::ERROR_UNKNOWN;
};

// ==========================================================================
// WAV header builder
// ==========================================================================

// RIFF/WAV header layout (44 bytes, little-endian)
#pragma pack(push, 1)
struct WavHeader {
    char     chunkID[4]     = {'R','I','F','F'};
    uint32_t chunkSize      = 0;
    char     format[4]      = {'W','A','V','E'};
    char     subchunk1ID[4] = {'f','m','t',' '};
    uint32_t subchunk1Size  = 16;
    uint16_t audioFormat    = 1;   // PCM
    uint16_t numChannels    = 1;
    uint32_t sampleRate     = 44100;
    uint32_t byteRate       = 0;
    uint16_t blockAlign     = 0;
    uint16_t bitsPerSample  = 16;
    char     subchunk2ID[4] = {'d','a','t','a'};
    uint32_t subchunk2Size  = 0;
};
#pragma pack(pop)

/*static*/
std::vector<uint8_t> TtsEngine::buildWavHeader(int32_t sample_rate,
                                                int32_t bits_per_sample,
                                                int32_t channels,
                                                int32_t data_size)
{
    WavHeader h;
    h.numChannels   = static_cast<uint16_t>(channels);
    h.sampleRate    = static_cast<uint32_t>(sample_rate);
    h.bitsPerSample = static_cast<uint16_t>(bits_per_sample);
    h.blockAlign    = static_cast<uint16_t>(channels * bits_per_sample / 8);
    h.byteRate      = static_cast<uint32_t>(sample_rate * channels * bits_per_sample / 8);
    h.subchunk2Size = static_cast<uint32_t>(data_size);
    h.chunkSize     = 36 + static_cast<uint32_t>(data_size);

    std::vector<uint8_t> hdr(sizeof(WavHeader));
    std::memcpy(hdr.data(), &h, sizeof(WavHeader));
    return hdr;
}

// ==========================================================================
// TtsEngine
// ==========================================================================

TtsEngine::TtsEngine(const std::string& model_path)
    : model_path_(model_path),
      collector_(std::make_unique<AudioCollector>()) {}

TtsEngine::~TtsEngine() {
    shutdown();
}

// --------------------------------------------------------------------------
// initialize – load the model with default config
// --------------------------------------------------------------------------
void TtsEngine::initialize() {
    std::lock_guard<std::mutex> lock(mu_);
    doInit(current_config_);
}

// --------------------------------------------------------------------------
// doInit – (re-)initialise the SDK (must hold mu_)
// --------------------------------------------------------------------------
void TtsEngine::doInit(const EngineConfig& cfg) {
    // Tear down any existing instance first
    if (initialized_) {
        engine_->deInit();
        initialized_ = false;
    }

    engine_ = TTS::TTSEngine::getInstance();
    engine_->registerResultCallback(collector_.get());

    TTS::TTSConfig tts_cfg = TTS::TTSConfig::Builder()
        .setConfig(TTS::ConfigKey::KEY_MODEL_PATH,    model_path_)
        .setConfig(TTS::ConfigKey::KEY_LANGUAGE,      cfg.language)
        .setConfig(TTS::ConfigKey::KEY_AUDIO_ENCODING,
                   std::to_string(static_cast<int>(cfg.encoding)))
        .setConfig(TTS::ConfigKey::KEY_SPEAKING_RATE,
                   std::to_string(cfg.speed))
        .setConfig(TTS::ConfigKey::KEY_PITCH,
                   std::to_string(cfg.pitch))
        .setConfig(TTS::ConfigKey::KEY_VOLUME_GAIN,
                   std::to_string(cfg.volume_gain))
        .setConfig(TTS::ConfigKey::KEY_SAMPLE_RATE,
                   std::to_string(cfg.sample_rate))
        .build();

    std::cout << "[TtsEngine] init: lang=" << cfg.language
              << " enc=" << static_cast<int>(cfg.encoding)
              << " rate=" << cfg.speed
              << " sr=" << cfg.sample_rate << "\n";

    const int32_t ret = engine_->init(&tts_cfg);
    if (ret != 0) {
        throw std::runtime_error(
            "[TtsEngine] TTSEngine::init() failed with code " +
            std::to_string(ret) + ". Check model path: " + model_path_);
    }

    current_config_ = cfg;
    initialized_    = true;
    std::cout << "[TtsEngine] Initialized successfully.\n";
}

// --------------------------------------------------------------------------
// shutdown
// --------------------------------------------------------------------------
void TtsEngine::doShutdown() noexcept {
    if (initialized_ && engine_) {
        engine_->stop();
        engine_->deInit();
        initialized_ = false;
    }
}

void TtsEngine::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    doShutdown();
}

bool TtsEngine::isReady() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return initialized_;
}

// --------------------------------------------------------------------------
// synthesize – core synthesis call (blocking)
//
// Flow:
//   1. Build EngineConfig from request
//   2. If config changed, call doInit() to reload with new config
//   3. Reset the AudioCollector
//   4. Call engine_->start(text)  ← synchronous; callbacks fire here
//   5. Check for errors
//   6. Optionally prepend WAV header
//   7. Return SynthesisResult
// --------------------------------------------------------------------------
SynthesisResult TtsEngine::synthesize(const SynthesisRequest& req) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!initialized_) {
        return SynthesisResult{.success = false,
                               .error   = "TtsEngine not initialized"};
    }

    // Build the desired engine config from the request
    EngineConfig desired;
    desired.language    = req.language;
    desired.encoding    = req.encoding;
    desired.speed       = req.speed;
    desired.pitch       = req.pitch;
    desired.volume_gain = req.volume_gain;
    desired.sample_rate = req.sample_rate;

    // Re-initialise only when config has changed
    if (desired != current_config_) {
        std::cout << "[TtsEngine] Config changed – reinitialising...\n";
        doInit(desired);
    }

    // Reset collector for this request
    collector_->reset();

    const auto t_start = std::chrono::steady_clock::now();

    // start() is synchronous – all callbacks fire before it returns
    const int32_t ret = engine_->start(req.text);

    const auto t_end = std::chrono::steady_clock::now();
    const uint64_t elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_end - t_start).count();

    // Check for errors
    if (ret != 0) {
        return SynthesisResult{
            .success         = false,
            .error           = "TTSEngine::start() returned " + std::to_string(ret),
            .process_time_ms = elapsed_ms};
    }
    if (collector_->hasError()) {
        return SynthesisResult{
            .success         = false,
            .error           = AudioCollector::errorName(collector_->errorType()),
            .process_time_ms = elapsed_ms};
    }

    // Build result
    SynthesisResult result;
    result.success         = true;
    result.sample_rate     = collector_->sampleRate();
    result.bits_per_sample = collector_->bitsPerSample();
    result.channels        = collector_->channels();
    result.process_time_ms = elapsed_ms;

    const auto& raw = collector_->audio();

    if (req.add_wav_header &&
        req.encoding == AudioEncoding::LINEAR16) {
        // Prepend RIFF/WAV header
        auto hdr = buildWavHeader(result.sample_rate,
                                  result.bits_per_sample,
                                  result.channels,
                                  static_cast<int32_t>(raw.size()));
        result.audio.reserve(hdr.size() + raw.size());
        result.audio.insert(result.audio.end(), hdr.begin(), hdr.end());
        result.audio.insert(result.audio.end(), raw.begin(),  raw.end());
    } else {
        result.audio = raw;
    }

    std::cout << "[TtsEngine] Synthesized " << result.audio.size()
              << " bytes in " << elapsed_ms << " ms\n";

    // Stop the engine after each synthesis (required by SDK)
    engine_->stop();

    return result;
}
