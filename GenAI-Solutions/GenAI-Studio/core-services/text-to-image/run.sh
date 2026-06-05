#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
# ---------------------------------------------------------------------
#
# run.sh – Start the Image-Generation service inside the container.
#
# Exposes:
#   GET  /health
#   GET  /v1/models
#   GET  /v1/images/generations/params
#   POST /v1/images/generations
#   POST /generate
#
# Environment variable overrides:
#   MODEL_DIR            Model/runtime directory (default: /opt/runtime)
#   QAIRT_LIB_DIR        QAIRT runtime library dir (default: $MODEL_DIR)
#   QAIRT_ADSP_LIB_DIR   ADSP skel lib dir (default: $QAIRT_LIB_DIR)
#   PORT                 HTTP port (default: 8084)
#   IMAGE_GEN_CACHE_SIZE Response cache size (default: 16)
#   IMAGE_GEN_API_KEY    Optional Bearer auth key
# ---------------------------------------------------------------------

set -euo pipefail

MODEL_DIR="${MODEL_DIR:-/opt/runtime}"
QAIRT_LIB_DIR="${QAIRT_LIB_DIR:-${MODEL_DIR}}"
QAIRT_ADSP_LIB_DIR="${QAIRT_ADSP_LIB_DIR:-${QAIRT_LIB_DIR}}"
PORT="${PORT:-8084}"
CACHE_SIZE="${IMAGE_GEN_CACHE_SIZE:-16}"
API_KEY="${IMAGE_GEN_API_KEY:-}"
QAIRT_VERSION_HINT="${QAIRT_VERSION_HINT:-runtime=2.45.0.260326}"

echo "============================================================"
echo " Image-Generation service starting"
echo "============================================================"
echo " Model dir      : ${MODEL_DIR}"
echo " QAIRT lib dir  : ${QAIRT_LIB_DIR}"
echo " QAIRT ADSP dir : ${QAIRT_ADSP_LIB_DIR}"
echo " QAIRT hint     : ${QAIRT_VERSION_HINT}"
echo " Port           : ${PORT}"
echo " Cache size     : ${CACHE_SIZE}"
if [ -n "${API_KEY}" ]; then
    echo " Auth           : enabled"
else
    echo " Auth           : disabled"
fi
echo "============================================================"

if [ ! -d "${MODEL_DIR}" ]; then
    echo "[run.sh] ERROR: Model/runtime directory not found: ${MODEL_DIR}"
    exit 1
fi

if [ ! -d "${QAIRT_LIB_DIR}" ]; then
    echo "[run.sh] ERROR: QAIRT lib directory not found: ${QAIRT_LIB_DIR}"
    exit 1
fi

resolve_context_file() {
    local kind="$1"
    shift
    local candidate
    local file
    shopt -s nullglob
    for candidate in "$@"; do
        if [[ "${candidate}" == *"*"* ]]; then
            for file in "${MODEL_DIR}"/${candidate}; do
                if [ -e "${file}" ]; then
                    basename "${file}"
                    return 0
                fi
            done
        else
            if [ -e "${MODEL_DIR}/${candidate}" ]; then
                echo "${candidate}"
                return 0
            fi
        fi
    done
    shopt -u nullglob
    echo "[run.sh] ERROR: Missing ${kind} context in ${MODEL_DIR}. Tried: $*" >&2
    return 1
}

TEXT_CONTEXT_FILE="$(resolve_context_file text_encoder \
    text_encoder.bin \
    text_encoder_qairt_context.bin \
    stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin \
    text_encoder*.bin)" || exit 1
UNET_CONTEXT_FILE="$(resolve_context_file unet \
    unet.bin \
    unet_qairt_context.bin \
    stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin \
    unet*.bin)" || exit 1
VAE_CONTEXT_FILE="$(resolve_context_file vae \
    vae.bin \
    vae_decoder.bin \
    vae_qairt_context.bin \
    stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin \
    vae*.bin)" || exit 1

echo "[run.sh] Context files:"
echo "         text: ${TEXT_CONTEXT_FILE}"
echo "         unet: ${UNET_CONTEXT_FILE}"
echo "         vae : ${VAE_CONTEXT_FILE}"

if [ -e "${MODEL_DIR}/cpp_sd21_qnn_direct/sd21_qnn_cpp_direct" ]; then
    echo "[run.sh] Direct runner found in runtime dir."
elif [ -x "/usr/bin/sd21_qnn_cpp_direct" ]; then
    echo "[run.sh] Direct runner found in image at /usr/bin/sd21_qnn_cpp_direct."
else
    echo "[run.sh] Direct runner not found; service will use in-process QNN path."
fi

TOKENIZER_DIR="${TOKENIZER_DIR:-${MODEL_DIR}/tokenizer}"
if [ ! -f "${TOKENIZER_DIR}/vocab.json" ] || [ ! -f "${TOKENIZER_DIR}/merges.txt" ]; then
    echo "[run.sh] ERROR: Tokenizer files not found under: ${TOKENIZER_DIR}"
    exit 1
fi

for req in \
    "${QAIRT_LIB_DIR}/libQnnHtp.so" \
    "${QAIRT_LIB_DIR}/libQnnSystem.so"
do
    if [ ! -e "${req}" ]; then
        echo "[run.sh] ERROR: Required QAIRT library missing: ${req}"
        exit 1
    fi
done

if [ ! -e "${MODEL_DIR}/libQnnHtpV73Skel.so" ] && \
   [ ! -e "${QAIRT_ADSP_LIB_DIR}/libQnnHtpV73Skel.so" ]; then
    echo "[run.sh] ERROR: Missing libQnnHtpV73Skel.so in both:"
    echo "                ${MODEL_DIR}"
    echo "                ${QAIRT_ADSP_LIB_DIR}"
    exit 1
fi

export SD21_TEXT_CONTEXT="${TEXT_CONTEXT_FILE}"
export SD21_UNET_CONTEXT="${UNET_CONTEXT_FILE}"
export SD21_VAE_CONTEXT="${VAE_CONTEXT_FILE}"
export LD_LIBRARY_PATH="${QAIRT_LIB_DIR}:${MODEL_DIR}:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export ADSP_LIBRARY_PATH="${QAIRT_ADSP_LIB_DIR};${QAIRT_LIB_DIR};${MODEL_DIR}${ADSP_LIBRARY_PATH:+;${ADSP_LIBRARY_PATH}}"
export QAIRT_ADSP_LIB_DIR

echo "[run.sh] Required files found. Starting image-generation-service..."

exec /usr/bin/image-generation-service \
    --model-dir "${MODEL_DIR}" \
    --tokenizer-dir "${TOKENIZER_DIR}" \
    --port "${PORT}" \
    --cache-size "${CACHE_SIZE}" \
    ${API_KEY:+--api-key "${API_KEY}"}
