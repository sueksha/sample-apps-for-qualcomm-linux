# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Speech-to-Text (asr-service) - optimized multi-stage Docker build
#
# ─── Size target ─────────────────────────────────────────────────────────────
#   < 700 MB  (ubuntu-runtime:24.04 base = 688 MB + asr-service + whisper libs)
#
# ─── Two stages ──────────────────────────────────────────────────────────────
#   builder  : genai-build-base:latest (cmake + gcc + nlohmann + httplib)
#              Compiles asr-service binary. NOT shipped.
#
#   runtime  : ubuntu-runtime:24.04 (minimal flat layer, no firmware)
#              Ships: asr-service binary + whisper .so libs + VAD assets
#
# Prerequisites
#   Run from repo root via scripts/build-all.sh (handles all prereqs):
#     bash scripts/build-all.sh
#
#   Or manually:
#     bash scripts/pull-ubuntu-arm64.sh
#     bash scripts/create-runtime-base.sh
#     DOCKER_BUILDKIT=1 docker build -f Dockerfile.build-base -t genai-build-base:latest .
#     DOCKER_BUILDKIT=1 docker build -t speech-to-text:latest core-services/speech-to-text/
# =============================================================================

# syntax=docker/dockerfile:1.7

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 1: Builder — compile asr-service
# ═══════════════════════════════════════════════════════════════════════════════
FROM genai-build-base:latest AS builder

WORKDIR /app/core-services/speech-to-text

# Copy C++ source files
COPY src/ ./src/

# Copy minimal Whisper SDK slice (headers + linux libs + assets)
COPY whisper_sdk/ ./whisper_sdk/

# Reuse pre-downloaded httplib header from genai-build-base (no wget needed)
RUN mkdir -p ./src/cpp-httplib && \
    cp /opt/headers/httplib/httplib.h ./src/cpp-httplib/httplib.h

# Build asr-service binary
RUN rm -rf ./src/build && \
    mkdir -p ./src/build && \
    cd ./src/build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DWHISPER_SDK_ROOT=/app/core-services/speech-to-text/whisper_sdk && \
    make -j"$(nproc)"

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 2: Runtime — minimal flat image (target: < 700 MB)
#
# Uses ubuntu-runtime:24.04 — pre-built minimal base (688 MB flat layer).
# Created by: bash scripts/create-runtime-base.sh
#
# Contains: qcom-fastrpc1, libstdc++6, libatomic1, curl, bash (no firmware)
# ═══════════════════════════════════════════════════════════════════════════════
FROM ubuntu-runtime:24.04

# Whisper .so libs are in /usr/lib; QNN libs injected via CDI device
ENV LD_LIBRARY_PATH=/usr/lib

# Copy compiled asr-service binary
COPY --link --chmod=0755 --from=builder /app/core-services/speech-to-text/src/build/asr-service /usr/bin/asr-service

# Copy Whisper runtime shared libraries to /usr/lib/
COPY --link --from=builder \
    /app/core-services/speech-to-text/whisper_sdk/libs/npu/rpc_libraries/linux/whisper_all_quantized/ \
    /usr/lib/

# Copy Whisper VAD assets into the image so AIHub-only model directories
# (encoder/decoder/vocab) can still run without manual VAD file copy.
COPY --link --from=builder \
    /app/core-services/speech-to-text/whisper_sdk/libs/npu/rpc_libraries/assets/ \
    /opt/asr-assets/

WORKDIR /app/
COPY --link --chmod=0755 run.sh /app/run.sh

# Port 8081 — asr-service HTTP server
EXPOSE 8081

# ─── Runtime volume mounts (provided by docker run / docker compose) ──────────
# -v /path/to/whisper/models:/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075/
#     Provides: encoder_model_htp.bin, decoder_model_htp.bin, vocab.bin
#     Optional: libnnvad_model.so (fallback copy is embedded in image)
# --device qualcomm.com/device=cdi-hw-acc → QNN libs + /dev/fastrpc-cdsp
CMD ["./run.sh"]
