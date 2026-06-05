#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# download-qairt-sdk.sh
#
# Prepare a minimal QAIRT SDK slice under repo-root qairt-sdk/ for Docker
# build contexts that expect:
#   qairt-sdk/include/Genie
#   qairt-sdk/include/QNN
#   qairt-sdk/lib/aarch64-oe-linux-gcc11.2
#
# Usage:
#   bash scripts/download-qairt-sdk.sh --service base
#   QAIRT_SDK_ROOT=/opt/qairt/current bash scripts/download-qairt-sdk.sh --service text-to-text
#
# Notes:
# - "download" is kept for compatibility with older docs; this script only
#   copies from an existing local QAIRT installation.
# - FORCE_DOWNLOAD=1 forces recreation of the destination slice.
# ---------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SERVICE="base"
DEST_DIR=""
FORCE_DOWNLOAD="${FORCE_DOWNLOAD:-0}"
QAIRT_SDK_ROOT="${QAIRT_SDK_ROOT:-}"

usage() {
    cat <<USAGE
Usage: $0 [--service <name>] [--dest <path>] [--help]

Options:
  --service <name>   Supported: base, text-to-text, image-to-text,
                     speech-to-text, text-to-image
                     (legacy aliases: text-generation, image-generation)
                     (default: base)
  --dest <path>      Override output directory (default: <repo>/qairt-sdk)
  --help             Show this help

Environment variables:
  QAIRT_SDK_ROOT     Source QAIRT root (must contain include/ and lib/)
  FORCE_DOWNLOAD=1   Recreate output even if destination already exists
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --service)
            SERVICE="${2:-}"
            shift 2
            ;;
        --dest)
            DEST_DIR="${2:-}"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "[download-qairt-sdk] ERROR: unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

normalize_service() {
    local in="$1"
    echo "$in" | tr '[:upper:]' '[:lower:]'
}

SERVICE="$(normalize_service "$SERVICE")"
case "$SERVICE" in
    text-generation) SERVICE="text-to-text" ;;
    image-generation) SERVICE="text-to-image" ;;
esac
case "$SERVICE" in
    base|text-to-text|speech-to-text|image-to-text|text-to-image)
        ;;
    *)
        echo "[download-qairt-sdk] ERROR: unsupported service: $SERVICE" >&2
        exit 1
        ;;
esac

if [[ -z "$DEST_DIR" ]]; then
    # Keep one canonical SDK slice for all builders.
    DEST_DIR="${REPO_ROOT}/qairt-sdk"
fi

has_required_layout() {
    local root="$1"
    [[ -d "$root/include/Genie" ]] && [[ -d "$root/include/QNN" ]] && [[ -d "$root/lib" ]]
}

resolve_source_root() {
    local candidate

    if [[ -n "$QAIRT_SDK_ROOT" ]]; then
        if has_required_layout "$QAIRT_SDK_ROOT"; then
            echo "$QAIRT_SDK_ROOT"
            return 0
        fi
        echo "[download-qairt-sdk] WARNING: QAIRT_SDK_ROOT does not have expected layout: $QAIRT_SDK_ROOT" >&2
    fi

    for candidate in "/opt/qairt/current" "/opt/qairt"; do
        if has_required_layout "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

SRC_ROOT="$(resolve_source_root || true)"
if [[ -z "$SRC_ROOT" ]]; then
    echo "[download-qairt-sdk] ERROR: could not find QAIRT source root." >&2
    echo "[download-qairt-sdk] Set QAIRT_SDK_ROOT to a path containing include/Genie, include/QNN, and lib/." >&2
    exit 1
fi

if [[ -d "$DEST_DIR" && "$FORCE_DOWNLOAD" != "1" ]]; then
    echo "[download-qairt-sdk] Reusing existing SDK slice: $DEST_DIR"
    exit 0
fi

echo "============================================================"
echo " QAIRT SDK Slice Preparation"
echo "============================================================"
echo " Service   : $SERVICE"
echo " Source    : $SRC_ROOT"
echo " Dest      : $DEST_DIR"
echo " Force     : $FORCE_DOWNLOAD"
echo "============================================================"

TMP_DIR="${DEST_DIR}.tmp.$$"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/include" "$TMP_DIR/lib"

cp -a "$SRC_ROOT/include/Genie" "$TMP_DIR/include/"
cp -a "$SRC_ROOT/include/QNN" "$TMP_DIR/include/"

if [[ -d "$SRC_ROOT/lib/aarch64-oe-linux-gcc11.2" ]]; then
    cp -a "$SRC_ROOT/lib/aarch64-oe-linux-gcc11.2" "$TMP_DIR/lib/"
else
    echo "[download-qairt-sdk] WARNING: missing $SRC_ROOT/lib/aarch64-oe-linux-gcc11.2" >&2
    echo "[download-qairt-sdk] Copying full lib/ as fallback." >&2
    cp -a "$SRC_ROOT/lib"/* "$TMP_DIR/lib/"
fi

if [[ ! -f "$TMP_DIR/lib/aarch64-oe-linux-gcc11.2/libGenie.so" ]]; then
    echo "[download-qairt-sdk] WARNING: libGenie.so was not found under copied libs." >&2
fi

rm -rf "$DEST_DIR"
mv "$TMP_DIR" "$DEST_DIR"

echo "[download-qairt-sdk] SDK slice ready: $DEST_DIR"
du -sh "$DEST_DIR" | sed 's#^#[download-qairt-sdk] Size: #' 
