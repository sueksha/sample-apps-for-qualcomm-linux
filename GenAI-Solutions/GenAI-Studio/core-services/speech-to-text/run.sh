#!/bin/bash
# =============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# run.sh – Container entrypoint for Speech-To-Text (asr-service)
#
# Environment variables:
#   MODEL_PATH      Path to Whisper model directory
#   ASR_VAD_PATH    Optional explicit VAD model path
#   ASR_ASSETS_DIR  Fallback VAD asset directory inside image
# =============================================================================

set -euo pipefail

MODEL_PATH="${MODEL_PATH:-/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075/}"
QNN_LIB_DIR="${QNN_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"
QNN_SKEL_PATH="${QNN_SKEL_PATH:-}"
RUNTIME_LIB_DIR="${RUNTIME_LIB_DIR:-/tmp/asr-runtime-libs}"
ASR_VAD_PATH="${ASR_VAD_PATH:-}"
ASR_ASSETS_DIR="${ASR_ASSETS_DIR:-/opt/asr-assets}"
QAIRT_VERSION_HINT="${QAIRT_VERSION_HINT:-runtime=2.45.0.260326}"

# Ensure model path has trailing slash for Whisper SDK path assembly.
if [[ -n "${MODEL_PATH}" && "${MODEL_PATH}" != */ ]]; then
    MODEL_PATH="${MODEL_PATH}/"
fi

# Prepare writable runtime overlay for transient symlinks (do not mutate mounted model dir).
mkdir -p "${RUNTIME_LIB_DIR}"

# FastRPC ADSP search path is semicolon-separated in Qualcomm runtime.
# Keep QNN runtime directory first so an older skel in model dir does not take precedence.
export ADSP_LIBRARY_PATH="${RUNTIME_LIB_DIR};${QNN_LIB_DIR};${MODEL_PATH};/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1${ADSP_LIBRARY_PATH:+;${ADSP_LIBRARY_PATH}}"

# Build LD path with runtime overlay first, then QNN libs, model dir, and system libs.
export LD_LIBRARY_PATH="${RUNTIME_LIB_DIR}:${QNN_LIB_DIR}:${MODEL_PATH}:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH:-}"

echo "============================================================"
echo " Speech-To-Text (asr-service) starting"
echo "============================================================"
echo " Model path : ${MODEL_PATH}"
echo " QNN libs   : ${QNN_LIB_DIR}"
echo " QAIRT hint : ${QAIRT_VERSION_HINT}"
echo " Runtime dir: ${RUNTIME_LIB_DIR}"
if [[ -n "${QNN_SKEL_PATH}" ]]; then
    echo " QNN skel   : ${QNN_SKEL_PATH}"
fi
echo " Assets dir : ${ASR_ASSETS_DIR}"
echo " Port       : 8081"
echo " LD_LIBRARY_PATH  : ${LD_LIBRARY_PATH}"
echo " ADSP_LIBRARY_PATH: ${ADSP_LIBRARY_PATH}"
echo "============================================================"

for req in \
    "${QNN_LIB_DIR}/libQnnHtp.so" \
    "${QNN_LIB_DIR}/libQnnSystem.so"; do
    if [[ ! -e "${req}" ]]; then
        echo "[run.sh] ERROR: required QNN runtime library missing: ${req}"
        exit 1
    fi
done

if [[ ! -e /opt/host-libs/libcdsprpc.so.1 && \
      ! -e /opt/host-libs/libcdsprpc.so && \
      ! -e /opt/host-libs/libcdsprpc.so.1.0.0 && \
      ! -e /usr/lib/libcdsprpc.so.1 && \
      ! -e /usr/lib/libcdsprpc.so && \
      ! -e /usr/lib/libcdsprpc.so.1.0.0 && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1 && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1.0.0 ]]; then
    echo "[run.sh] ERROR: libcdsprpc not found in /opt/host-libs or system lib paths."
    echo "[run.sh] Check HOST_RPC_LIB_DIR in docker-compose/.env."
    exit 1
fi

resolve_model_file() {
    local canonical_name="$1"
    local prefix_fallback="$2"
    local canonical_path="${MODEL_PATH%/}/${canonical_name}"
    if [[ -f "${canonical_path}" ]]; then
        echo "${canonical_path}"
        return
    fi
    if [[ -n "${prefix_fallback}" ]]; then
        local found
        found="$(find "${MODEL_PATH}" -maxdepth 1 -type f -name "${prefix_fallback}*.bin" | head -n1 || true)"
        if [[ -n "${found}" ]]; then
            echo "${found}"
            return
        fi
    fi
    echo ""
}

ENCODER_FILE="$(resolve_model_file "encoder.bin" "whisper_tiny-encoder-")"
DECODER_FILE="$(resolve_model_file "decoder.bin" "whisper_tiny-decoder-")"
VOCAB_FILE="${MODEL_PATH%/}/vocab.bin"

if [[ -z "${ENCODER_FILE}" || -z "${DECODER_FILE}" || ! -f "${VOCAB_FILE}" ]]; then
    echo ""
    echo "[run.sh] ERROR: required model artifacts not found under ${MODEL_PATH}"
    echo "[run.sh] Required:"
    echo "[run.sh]   encoder.bin (or whisper_tiny-encoder-*.bin)"
    echo "[run.sh]   decoder.bin (or whisper_tiny-decoder-*.bin)"
    echo "[run.sh]   vocab.bin"
    echo "[run.sh] Tip: see core-services/speech-to-text/MODEL_SETUP.md for the approved model download and layout steps."
    exit 1
fi

if [[ -z "${ASR_VAD_PATH}" ]]; then
    for candidate in \
        "${MODEL_PATH%/}/libnnvad_model.so" \
        "${ASR_ASSETS_DIR}/libnnvad_model.so"; do
        if [[ -f "${candidate}" ]]; then
            ASR_VAD_PATH="${candidate}"
            break
        fi
    done
fi

if [[ -z "${ASR_VAD_PATH}" || ! -f "${ASR_VAD_PATH}" ]]; then
    echo "[run.sh] ERROR: no VAD model found."
    echo "[run.sh] Expected one of:"
    echo "[run.sh]   ${MODEL_PATH%/}/libnnvad_model.so"
    echo "[run.sh]   ${ASR_ASSETS_DIR}/libnnvad_model.so"
    exit 1
fi

# Resolve a V73 skel into writable runtime overlay to avoid touching host-mounted model files.
SKEL_SOURCE=""
if [[ -n "${QNN_SKEL_PATH}" && -f "${QNN_SKEL_PATH}" ]]; then
    SKEL_SOURCE="${QNN_SKEL_PATH}"
elif [[ -f "${QNN_LIB_DIR}/libQnnHtpV73Skel.so" ]]; then
    SKEL_SOURCE="${QNN_LIB_DIR}/libQnnHtpV73Skel.so"
elif [[ -f "${MODEL_PATH}/libQnnHtpV73Skel.so" ]]; then
    SKEL_SOURCE="${MODEL_PATH}/libQnnHtpV73Skel.so"
fi

if [[ -n "${SKEL_SOURCE}" ]]; then
    ln -sf "${SKEL_SOURCE}" "${RUNTIME_LIB_DIR}/libQnnHtpV73Skel.so" || true
    ln -sf "${RUNTIME_LIB_DIR}/libQnnHtpV73Skel.so" "${RUNTIME_LIB_DIR}/libQnnHtpV73Skel.so.2" || true
fi

export ASR_VAD_PATH

echo "[run.sh] Encoder file : ${ENCODER_FILE}"
echo "[run.sh] Decoder file : ${DECODER_FILE}"
echo "[run.sh] Vocab file   : ${VOCAB_FILE}"
echo "[run.sh] VAD file     : ${ASR_VAD_PATH}"
echo "[run.sh] Model files found. Starting asr-service..."

exec asr-service \
    --model-path "${MODEL_PATH}" \
    --vad-model-path "${ASR_VAD_PATH}" \
    --port 8081
