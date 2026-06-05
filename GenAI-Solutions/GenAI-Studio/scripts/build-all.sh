#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# build-all.sh
#
# Canonical wrapper for shared base + all service image builds.
#
# Builds by default:
#   - ubuntu:24.04 (via pull-ubuntu-arm64.sh if needed)
#   - ubuntu-runtime:24.04 (via create-runtime-base.sh if needed)
#   - genai-build-base:latest
#   - text-to-text:latest
#   - image-to-text:responses-v1
#   - speech-to-text:latest
#   - text-to-speech:latest
#   - text-to-image:latest
#   - orchestrator:latest
#
# Usage:
#   bash scripts/build-all.sh
#   WHISPER_SDK_ROOT=/path/to/whisper-sdk bash scripts/build-all.sh
#   bash scripts/build-all.sh --skip-stt
#   bash scripts/build-all.sh --force-base-rebuild
# ---------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SKIP_BASE=0
SKIP_TEXTGEN=0
SKIP_I2T=0
SKIP_STT=0
SKIP_TTS=0
SKIP_IMAGEGEN=0
SKIP_ORCHESTRATOR=0
FORCE_BASE_REBUILD="${FORCE_BASE_REBUILD:-0}"
AUTO_PREPARE_QAIRT_SDK="${AUTO_PREPARE_QAIRT_SDK:-1}"

usage() {
    cat <<USAGE
Usage: $0 [--skip-base] [--skip-textgen] [--skip-i2t] [--skip-stt] [--skip-tts] [--skip-imagegen] [--skip-orchestrator]
          [--force-base-rebuild]

Environment:
  WHISPER_SDK_ROOT   Required for STT build if core-services/speech-to-text/whisper_sdk or whisper-sdk is absent
  FORCE_BASE_REBUILD 1 forces base image rebuild even if already present
  AUTO_PREPARE_QAIRT_SDK
                     1 (default) auto-runs scripts/download-qairt-sdk.sh --service base
                     when qairt-sdk/ is missing/incomplete
USAGE
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[build-all] ERROR: required command not found: $1" >&2
        exit 1
    fi
}

has_qairt_sdk_layout() {
    local root="${REPO_ROOT}/qairt-sdk"
    [[ -d "${root}/include/Genie" ]] && [[ -d "${root}/include/QNN" ]] && [[ -d "${root}/lib" ]]
}

ensure_qairt_sdk_slice() {
    if has_qairt_sdk_layout; then
        echo "[build-all] qairt-sdk layout OK"
        return 0
    fi
    if [[ "${AUTO_PREPARE_QAIRT_SDK}" != "1" ]]; then
        echo "[build-all] ERROR: qairt-sdk is missing/incomplete and AUTO_PREPARE_QAIRT_SDK=0" >&2
        echo "[build-all] Run: bash scripts/download-qairt-sdk.sh --service base" >&2
        exit 1
    fi
    echo "[build-all] qairt-sdk missing/incomplete; preparing from local QAIRT install..."
    bash "${REPO_ROOT}/scripts/download-qairt-sdk.sh" --service base
    if ! has_qairt_sdk_layout; then
        echo "[build-all] ERROR: qairt-sdk is still missing/incomplete after preparation" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-base)    SKIP_BASE=1; shift ;;
        --skip-textgen) SKIP_TEXTGEN=1; shift ;;
        --skip-i2t)     SKIP_I2T=1; shift ;;
        --skip-stt)     SKIP_STT=1; shift ;;
        --skip-tts)     SKIP_TTS=1; shift ;;
        --skip-imagegen) SKIP_IMAGEGEN=1; shift ;;
        --skip-orchestrator) SKIP_ORCHESTRATOR=1; shift ;;
        --force-base-rebuild) FORCE_BASE_REBUILD=1; shift ;;
        --help|-h)      usage; exit 0 ;;
        *)
            echo "[build-all] ERROR: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

cd "${REPO_ROOT}"
require_cmd docker
if ! docker compose version >/dev/null 2>&1; then
    echo "[build-all] ERROR: docker compose plugin is required" >&2
    exit 1
fi

echo "============================================================"
echo " GenAI Studio build-all"
echo "============================================================"
echo " Repo        : ${REPO_ROOT}"
echo " Skip base   : ${SKIP_BASE}"
echo " Skip textgen: ${SKIP_TEXTGEN}"
echo " Skip i2t    : ${SKIP_I2T}"
echo " Skip stt    : ${SKIP_STT}"
echo " Skip tts    : ${SKIP_TTS}"
echo " Skip imagegen: ${SKIP_IMAGEGEN}"
echo " Skip orchestrator: ${SKIP_ORCHESTRATOR}"
echo " Force base rebuild: ${FORCE_BASE_REBUILD}"
echo " Auto-prepare qairt-sdk: ${AUTO_PREPARE_QAIRT_SDK}"
echo "============================================================"

if [[ "${SKIP_BASE}" != "1" ]]; then
    ensure_qairt_sdk_slice

    if [[ "${FORCE_BASE_REBUILD}" == "1" ]] || ! docker image inspect ubuntu:24.04 >/dev/null 2>&1; then
        bash "${REPO_ROOT}/scripts/pull-ubuntu-arm64.sh"
    else
        echo "[build-all] ubuntu:24.04 already present; skipping pull"
    fi

    if [[ "${FORCE_BASE_REBUILD}" == "1" ]] || ! docker image inspect ubuntu-runtime:24.04 >/dev/null 2>&1; then
        bash "${REPO_ROOT}/scripts/create-runtime-base.sh"
    else
        echo "[build-all] ubuntu-runtime:24.04 already present; skipping rebuild"
    fi

    if [[ "${FORCE_BASE_REBUILD}" == "1" ]] || ! docker image inspect genai-build-base:latest >/dev/null 2>&1; then
        DOCKER_BUILDKIT=1 docker build \
            --progress=plain \
            -f "${REPO_ROOT}/Dockerfile.build-base" \
            -t genai-build-base:latest \
            "${REPO_ROOT}"
    else
        echo "[build-all] genai-build-base:latest already present; skipping rebuild"
    fi
fi

if [[ "${SKIP_TEXTGEN}" != "1" ]]; then
    bash "${REPO_ROOT}/core-services/text-to-text/build.sh"
fi

if [[ "${SKIP_I2T}" != "1" ]]; then
    bash "${REPO_ROOT}/core-services/image-to-text/build.sh"
fi

if [[ "${SKIP_STT}" != "1" ]]; then
    if [[ ! -d "${REPO_ROOT}/core-services/speech-to-text/whisper_sdk" && ! -d "${REPO_ROOT}/core-services/speech-to-text/whisper-sdk" && -z "${WHISPER_SDK_ROOT:-}" ]]; then
        echo "[build-all] ERROR: STT build needs WHISPER_SDK_ROOT when core-services/speech-to-text/whisper_sdk and whisper-sdk are absent." >&2
        echo "[build-all] Either export WHISPER_SDK_ROOT=<path> or run with --skip-stt." >&2
        exit 1
    fi
    if [[ -n "${WHISPER_SDK_ROOT:-}" ]]; then
        WHISPER_SDK_ROOT="${WHISPER_SDK_ROOT}" bash "${REPO_ROOT}/core-services/speech-to-text/build.sh"
    else
        bash "${REPO_ROOT}/core-services/speech-to-text/build.sh"
    fi
fi

if [[ "${SKIP_TTS}" != "1" ]]; then
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -t text-to-speech:latest \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS"
fi

if [[ "${SKIP_IMAGEGEN}" != "1" ]]; then
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -f "${REPO_ROOT}/core-services/text-to-image/Dockerfile" \
        -t text-to-image:latest \
        "${REPO_ROOT}"
fi

if [[ "${SKIP_ORCHESTRATOR}" != "1" ]]; then
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -t orchestrator:latest \
        "${REPO_ROOT}/core-services/orchestrator"
fi

echo "============================================================"
echo " build-all complete"
echo "============================================================"
