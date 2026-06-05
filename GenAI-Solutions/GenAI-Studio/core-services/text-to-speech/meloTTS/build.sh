#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# Build script for tts-service (C++ binary using melo_sdk).
#
# Usage:
#   bash build.sh [--clean] [--sdk-root <path>] [--build-type Debug|Release]
#   Example QPM3 SDK path: /opt/qcom/qpm/VoiceAI_TTS/1.1.1.0/melo_sdk
#
# The script cross-compiles for aarch64-linux-gnu when a toolchain file
# is present at src/aarch64-toolchain.cmake, otherwise it builds natively
# (useful for native aarch64 devices like RB3 Gen 2).
# ---------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/src"
BUILD_DIR="${SRC_DIR}/build"
MELO_SDK_ROOT="${SCRIPT_DIR}/melo_sdk"
BUILD_TYPE="Release"
CLEAN=0
TOOLCHAIN_FILE="${SRC_DIR}/aarch64-toolchain.cmake"

# ---- Parse arguments --------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --sdk-root)
            MELO_SDK_ROOT="$2"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--clean] [--sdk-root <path>] [--build-type Debug|Release]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
done

# ---- Clean ------------------------------------------------------------------
if [[ $CLEAN -eq 1 ]]; then
    echo "[build] Cleaning ${BUILD_DIR} ..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

# ---- CMake configure --------------------------------------------------------
CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DMELO_SDK_ROOT="${MELO_SDK_ROOT}"
)

# Use cross-compilation toolchain if available
if [[ -f "${TOOLCHAIN_FILE}" ]]; then
    echo "[build] Using toolchain: ${TOOLCHAIN_FILE}"
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}")
else
    echo "[build] No toolchain file found – building natively"
fi

echo "[build] Configuring ..."
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

# ---- Build ------------------------------------------------------------------
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "[build] Building with ${NPROC} parallel jobs ..."
cmake --build "${BUILD_DIR}" --parallel "${NPROC}"

echo ""
echo "[build] ✓ Build complete"
echo "[build]   Binary : ${BUILD_DIR}/tts-service"
