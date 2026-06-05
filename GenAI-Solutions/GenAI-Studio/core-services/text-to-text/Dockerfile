# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Text-to-Text (textgen_server) - self-contained multi-stage Docker build
#
# ─── Two stages ──────────────────────────────────────────────────────────────
#   builder  : genai-build-base:latest (cmake + gcc + Qualcomm dev headers)
#              Compiles textgen_server binary. This stage is NOT shipped.
#
#   runtime  : ubuntu:24.04 (fresh) + qcom-fastrpc1 ONLY
#              Ships ONLY: textgen_server binary + FastRPC runtime.
#              Does NOT include: cmake, gcc, firmware, dev headers.
#
# ─── Size comparison ─────────────────────────────────────────────────────────
#   Old (genai-runtime base) : ~1.75 GB  (includes firmware-qcom-hlosfw)
#   New (ubuntu:24.04 base)  : ~400 MB   (qcom-fastrpc1 + binary only)
#
# Prerequisites
#   Run from core-services/text-to-text/ via build.sh (handles all prereqs):
#     bash build.sh
#
#   Or manually:
#     bash ../../scripts/pull-ubuntu-arm64.sh          # get ubuntu:24.04
#     QAIRT_SDK_ROOT=/path/to/qairt/2.45.0.260326 \
#         bash ../../scripts/download-qairt-sdk.sh --service text-generation
#     DOCKER_BUILDKIT=1 docker build -f ../../Dockerfile.build-base -t genai-build-base:latest ../../
#     DOCKER_BUILDKIT=1 docker build -t text-to-text:latest .
# =============================================================================

# syntax=docker/dockerfile:1.7

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 1: Builder — compile textgen_server
# Uses genai-build-base (cmake + gcc + Qualcomm dev headers + nlohmann + httplib)
# This stage is discarded after build — NOT shipped in the final image.
# ═══════════════════════════════════════════════════════════════════════════════
FROM genai-build-base:latest AS builder

WORKDIR /app/core-services/text-to-text

# Copy C++ source files
COPY src/ ./src/

# QAIRT SDK (Genie headers + libGenie.so) is already baked into genai-build-base:latest
# at /opt/qairt/ — no need to COPY qairt-sdk/ here.
# Set by ENV QAIRT_SDK_ROOT=/opt/qairt in genai-build-base.

# Reuse pre-downloaded httplib header from genai-build-base (no wget needed)
RUN mkdir -p ./src/cpp-httplib && \
    cp /opt/headers/httplib/httplib.h ./src/cpp-httplib/httplib.h

# Build textgen_server binary
# QAIRT_SDK_ROOT=/opt/qairt is set in the base image ENV
RUN --mount=type=cache,target=/app/core-services/text-to-text/src/build,sharing=locked \
    mkdir -p ./src/build && \
    cd ./src/build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DQNN_ROOT="${QAIRT_SDK_ROOT}" && \
    make -j"$(nproc)" && \
    cp textgen_server /tmp/textgen_server && \
    strip --strip-unneeded /tmp/textgen_server || true

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 2: Runtime — minimal flat image (target: < 500 MB)
#
# Uses ubuntu-runtime:24.04 — a pre-built minimal base created by:
#   bash scripts/create-runtime-base.sh
#
# This base is created via docker export + docker import (flat single layer).
# It strips everything not needed at runtime:
#   linux-firmware (547 MB), linux-firmware-dragonwing (311 MB),
#   snapd (120 MB), python3-botocore (92 MB), docs, locales, man pages
#
# Contains ONLY:
#   qcom-fastrpc1, qcom-libdmabufheap (FastRPC runtime, no firmware)
#   libstdc++6, libgcc-s1, libatomic1, libc6 (C++ runtime)
#   curl, bash, ca-certificates
#
# NOT included (provided at runtime via CDI device / volume mounts):
#   libGenie.so  : from -v /opt/:/opt/
#   libQnnHtp.so : from --device qualcomm.com/device=cdi-hw-acc
#   firmware     : loaded by host kernel, not needed in container
# ═══════════════════════════════════════════════════════════════════════════════
FROM ubuntu-runtime:24.04 AS runtime

# LD_LIBRARY_PATH already set in ubuntu-runtime:24.04 base image
# libGenie.so  → /opt/genie_bundle  (volume mount: -v /opt/:/opt/)
# QNN libs     → /usr/lib           (CDI device injection)

# Copy ONLY the compiled binary from builder stage
# COPY --link: parallel layer extraction, better cache reuse
COPY --link --chmod=0755 --from=builder /tmp/textgen_server /usr/bin/textgen_server
RUN ln -sf /usr/bin/textgen_server /usr/bin/llamachat

WORKDIR /app/
COPY --link --chmod=0755 run.sh /app/run.sh

# Port 8088 — OpenAI-compatible HTTP server
EXPOSE 8088

# ─── Runtime volume mounts (provided by docker run / docker compose) ──────────
# -v /opt/:/opt/                          → libGenie.so + model bundle
# --device qualcomm.com/device=cdi-hw-acc → QNN libs + /dev/fastrpc-cdsp
CMD ["./run.sh"]
