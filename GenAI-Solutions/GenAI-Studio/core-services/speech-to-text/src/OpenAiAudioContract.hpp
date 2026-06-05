// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>

enum class OpenAiAudioRouteKind {
    Transcriptions,
    Translations,
    TranscriptionsStream
};

struct OpenAiAudioRequestParams {
    std::string model = "whisper-tiny";
    std::string language = "en";
    std::string response_format = "json";
    bool stream_mode = false;
};
