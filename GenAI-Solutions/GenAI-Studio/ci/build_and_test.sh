#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# CI validation entrypoint used by .github/workflows/ci.yml.
#
# This script runs repository-level sanity checks and writes a report to:
#   ci/ci_report.log
#
# Optional flags are accepted for compatibility with existing workflow args.
# ---------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPORT_PATH="${SCRIPT_DIR}/ci_report.log"

QNN_SDK=""
WHISPER_LIBS=""
CROW_DIR=""
ASIO_DIR=""
STB_DIR=""
TG_GATE_URL="${TG_GATE_URL:-http://127.0.0.1:8088}"
TG_GATE_EXPECTED_PASS="${TG_GATE_EXPECTED_PASS:-33}"
TG_GATE_TIMEOUT_SEC="${TG_GATE_TIMEOUT_SEC:-1200}"
TG_GATE_FAST_ENABLE="${TG_GATE_FAST_ENABLE:-1}"
TG_GATE_FAST_EXPECTED_PASS="${TG_GATE_FAST_EXPECTED_PASS:-20}"
TG_GATE_FAST_TIMEOUT_SEC="${TG_GATE_FAST_TIMEOUT_SEC:-300}"
TG_GATE_PROFILE="${TG_GATE_PROFILE:-full}"
TG_GATE_OVERRIDE="${CI_TG_GATE_OVERRIDE:-}"
TG_GATE_OVERRIDE_REASON="${CI_TG_GATE_OVERRIDE_REASON:-}"
TG_INTERNAL_API_KEY_FOR_TESTS="${TG_INTERNAL_API_KEY:-tg-internal-placeholder-key}"
TTS_GATE_ENABLE="${TTS_GATE_ENABLE:-1}"
TTS_GATE_URL="${TTS_GATE_URL:-http://127.0.0.1:8083}"
TTS_GATE_EXPECTED_PASS="${TTS_GATE_EXPECTED_PASS:-35}"
TTS_GATE_TIMEOUT_SEC="${TTS_GATE_TIMEOUT_SEC:-1800}"
TTS_PARITY_EXPECTED_PROMPTS="${TTS_PARITY_EXPECTED_PROMPTS:-12}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qnn-sdk)
            QNN_SDK="${2:-}"
            shift 2
            ;;
        --whisper-libs)
            WHISPER_LIBS="${2:-}"
            shift 2
            ;;
        --crow-dir)
            CROW_DIR="${2:-}"
            shift 2
            ;;
        --asio-dir)
            ASIO_DIR="${2:-}"
            shift 2
            ;;
        --stb-dir)
            STB_DIR="${2:-}"
            shift 2
            ;;
        --help|-h)
            cat <<USAGE
Usage: $0 [--qnn-sdk <path>] [--whisper-libs <path>] [--crow-dir <path>] [--asio-dir <path>] [--stb-dir <path>]
USAGE
            exit 0
            ;;
        *)
            echo "[ci] ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

mkdir -p "${SCRIPT_DIR}"
ARTIFACT_ROOT="${SCRIPT_DIR}/artifacts"
TG_ARTIFACT_DIR="${ARTIFACT_ROOT}/tg"
TTS_ARTIFACT_DIR="${ARTIFACT_ROOT}/tts"
mkdir -p "${TG_ARTIFACT_DIR}"
mkdir -p "${TTS_ARTIFACT_DIR}"
tg_compose_started=0

# Write full run log to report while keeping stdout visible.
exec > >(tee "${REPORT_PATH}") 2>&1

failures=0

log_section() {
    echo ""
    echo "============================================================"
    echo "[ci] $1"
    echo "============================================================"
}

mark_fail() {
    echo "[ci] FAIL: $1"
    failures=$((failures + 1))
}

mark_pass() {
    echo "[ci] PASS: $1"
}

mark_warn() {
    echo "[ci] WARN: $1"
}

check_exists() {
    local path="$1"
    if [[ -e "${REPO_ROOT}/${path}" ]]; then
        mark_pass "exists: ${path}"
    else
        mark_fail "missing: ${path}"
    fi
}

check_optional_path() {
    local label="$1"
    local value="$2"
    if [[ -z "${value}" ]]; then
        echo "[ci] INFO: ${label} not provided"
        return
    fi
    if [[ -e "${value}" ]]; then
        mark_pass "${label} path exists: ${value}"
    else
        mark_fail "${label} path does not exist: ${value}"
    fi
}

redact_tg_env_snapshot() {
    env | grep -E '^(TG_|QNN_|QAIRT_)' | sed -E 's/=.*$/=***REDACTED***/' | sort || true
}

collect_tg_diagnostics() {
    echo "[ci] INFO: collecting Text-Generation diagnostics in ${TG_ARTIFACT_DIR}"

    (cd "${REPO_ROOT}" && docker compose config > "${TG_ARTIFACT_DIR}/docker_compose_config.txt") || true
    docker ps -a > "${TG_ARTIFACT_DIR}/docker_ps_a.txt" 2>&1 || true
    docker logs --tail 800 text-to-text > "${TG_ARTIFACT_DIR}/docker_logs_text_to_text.txt" 2>&1 || true
    docker logs --tail 800 orchestrator > "${TG_ARTIFACT_DIR}/docker_logs_orchestrator.txt" 2>&1 || true

    curl -sS -i "${TG_GATE_URL}/health" > "${TG_ARTIFACT_DIR}/curl_health.txt" 2>&1 || true
    curl -sS -i "${TG_GATE_URL}/ready" > "${TG_ARTIFACT_DIR}/curl_ready.txt" 2>&1 || true
    curl -sS -i "${TG_GATE_URL}/v1/models" > "${TG_ARTIFACT_DIR}/curl_models.txt" 2>&1 || true
    curl -sS -i -H "X-Internal-API-Key: ${TG_INTERNAL_API_KEY_FOR_TESTS}" \
        "${TG_GATE_URL}/v1/internal/models" > "${TG_ARTIFACT_DIR}/curl_internal_models.txt" 2>&1 || true
    curl -sS -i -X POST "${TG_GATE_URL}/reload_model" \
        -H "Content-Type: application/json" \
        -H "X-Internal-API-Key: ${TG_INTERNAL_API_KEY_FOR_TESTS}" \
        -d '{"system_prompt":"ci-diagnostic","max_tokens":32}' \
        > "${TG_ARTIFACT_DIR}/curl_reload_model.txt" 2>&1 || true
}

cleanup_tg_gate_stack() {
    if [[ "${tg_compose_started:-0}" -eq 1 ]] && command -v docker >/dev/null 2>&1; then
        (cd "${REPO_ROOT}" && docker compose rm -sf text-to-text > "${TG_ARTIFACT_DIR}/compose_cleanup_text_to_text.log" 2>&1) || true
    fi
}

trap cleanup_tg_gate_stack EXIT

log_section "Environment"
echo "[ci] Repo        : ${REPO_ROOT}"
echo "[ci] QNN_SDK     : ${QNN_SDK:-<unset>}"
echo "[ci] WHISPER_LIBS: ${WHISPER_LIBS:-<unset>}"
echo "[ci] CROW_DIR    : ${CROW_DIR:-<unset>}"
echo "[ci] ASIO_DIR    : ${ASIO_DIR:-<unset>}"
echo "[ci] STB_DIR     : ${STB_DIR:-<unset>}"
echo "[ci] TG_GATE_PROFILE: ${TG_GATE_PROFILE}"
echo "[ci] TTS_GATE_ENABLE: ${TTS_GATE_ENABLE}"
echo "[ci] TTS_GATE_URL: ${TTS_GATE_URL}"

log_section "Required Files"
check_exists "README.md"
check_exists "docs/README.md"
check_exists "docs/setup/DEVICE_SETUP.md"
check_exists "versions.env"
check_exists "docker-compose.yml"
check_exists "Dockerfile.build-base"
check_exists "scripts/README.md"
check_exists "core-services/text-to-text/build.sh"
check_exists "core-services/speech-to-text/build.sh"
check_exists "core-services/image-to-text/build.sh"
check_exists "core-services/text-to-image/build.sh"
check_exists "core-services/text-to-speech/meloTTS/build.sh"
check_exists "core-services/orchestrator/device_app.py"
check_exists "scripts/compose-doctor.sh"
check_exists "scripts/validate-stack.sh"
check_exists "scripts/repro/compose_doctor.sh"
check_exists "scripts/repro/run_full_stack_smoke.sh"
check_exists "scripts/pull-ubuntu-arm64.sh"
check_exists "scripts/create-runtime-base.sh"
check_exists "scripts/download-qairt-sdk.sh"

log_section "Optional Toolchain Paths"
check_optional_path "QNN_SDK" "${QNN_SDK}"
check_optional_path "WHISPER_LIBS" "${WHISPER_LIBS}"
check_optional_path "CROW_DIR" "${CROW_DIR}"
check_optional_path "ASIO_DIR" "${ASIO_DIR}"
check_optional_path "STB_DIR" "${STB_DIR}"

log_section "Shell Script Syntax"
while IFS= read -r -d '' shfile; do
    rel="${shfile#${REPO_ROOT}/}"
    if bash -n "${shfile}"; then
        mark_pass "bash -n ${rel}"
    else
        mark_fail "bash syntax error in ${rel}"
    fi
done < <(find "${REPO_ROOT}" -type f -name '*.sh' -not -path '*/.git/*' -print0)

log_section "Python Syntax"
if command -v python3 >/dev/null 2>&1; then
    while IFS= read -r -d '' pyfile; do
        rel="${pyfile#${REPO_ROOT}/}"
        if python3 -m py_compile "${pyfile}"; then
            mark_pass "py_compile ${rel}"
        else
            mark_fail "python syntax error in ${rel}"
        fi
    done < <(find "${REPO_ROOT}" -type f -name '*.py' -not -path '*/.git/*' -print0)
else
    mark_fail "python3 not found"
fi

log_section "Docker Compose Validation"
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    if (cd "${REPO_ROOT}" && docker compose config >/dev/null); then
        mark_pass "docker compose config"
    else
        mark_fail "docker compose config failed"
    fi
else
    echo "[ci] INFO: docker/compose not available, skipping compose validation"
fi

log_section "Text-Generation Behavioral Gate"
redact_tg_env_snapshot > "${TG_ARTIFACT_DIR}/tg_env_snapshot.txt" || true

if [[ "${TG_GATE_PROFILE}" != "quick" && "${TG_GATE_PROFILE}" != "full" ]]; then
    mark_fail "TG_GATE_PROFILE must be 'quick' or 'full' (got '${TG_GATE_PROFILE}')"
fi

if [[ "${TG_GATE_OVERRIDE}" == "TECH_LEAD_APPROVED" ]]; then
    if [[ -z "${TG_GATE_OVERRIDE_REASON}" ]]; then
        mark_fail "CI_TG_GATE_OVERRIDE=TECH_LEAD_APPROVED requires CI_TG_GATE_OVERRIDE_REASON"
    else
        echo "[ci] WARNING: Text-Generation gate bypassed by tech lead override"
        echo "[ci] WARNING: reason=${TG_GATE_OVERRIDE_REASON}"
        mark_pass "Text-Generation behavioral gate override accepted"
    fi
else
    tg_gate_failed=0

    if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1; then
        mark_fail "docker compose is required for Text-Generation behavioral gate"
        tg_gate_failed=1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        mark_fail "python3 is required for Text-Generation behavioral gate"
        tg_gate_failed=1
    fi

    if [[ ${tg_gate_failed} -eq 0 ]]; then
        if (cd "${REPO_ROOT}" && bash core-services/text-to-text/build.sh > "${TG_ARTIFACT_DIR}/build_text_generation.log" 2>&1); then
            mark_pass "Text-Generation image build"
        else
            mark_fail "Text-Generation image build failed"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 ]]; then
        if (cd "${REPO_ROOT}" && docker compose up -d --no-deps text-to-text > "${TG_ARTIFACT_DIR}/compose_up_text_to_text.log" 2>&1); then
            mark_pass "docker compose up text-to-text"
            tg_compose_started=1
        else
            mark_fail "docker compose up text-to-text failed"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 ]]; then
        health_ok=0
        for attempt in $(seq 1 30); do
            health_code=$(curl -sS -m 5 -o "${TG_ARTIFACT_DIR}/health.json" -w "%{http_code}" "${TG_GATE_URL}/health" || true)
            echo "${health_code}" > "${TG_ARTIFACT_DIR}/health.status"
            if [[ "${health_code}" == "200" ]]; then
                health_ok=1
                break
            fi
            sleep 2
        done
        if [[ ${health_ok} -eq 1 ]]; then
            mark_pass "Text-Generation /health ready"
        else
            mark_fail "Text-Generation /health did not return 200"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 ]]; then
        ready_ok=0
        for pass_num in 1 2; do
            for attempt in $(seq 1 30); do
                ready_code=$(curl -sS -m 5 -o "${TG_ARTIFACT_DIR}/ready.json" -w "%{http_code}" "${TG_GATE_URL}/ready" || true)
                echo "${ready_code}" > "${TG_ARTIFACT_DIR}/ready.status"
                if [[ "${ready_code}" == "200" ]]; then
                    ready_ok=1
                    break
                fi
                sleep 2
            done
            if [[ ${ready_ok} -eq 1 ]]; then
                break
            fi
            echo "[ci] WARN: /ready probe failed (pass ${pass_num}); retrying infra once"
            (cd "${REPO_ROOT}" && docker compose restart text-to-text > "${TG_ARTIFACT_DIR}/compose_restart_text_to_text_${pass_num}.log" 2>&1) || true
            sleep 2
        done
        if [[ ${ready_ok} -eq 1 ]]; then
            mark_pass "Text-Generation /ready ready"
        else
            mark_fail "Text-Generation /ready did not return 200 after one infra retry"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 ]]; then
        models_code=$(curl -sS -m 10 -o "${TG_ARTIFACT_DIR}/models.json" -w "%{http_code}" "${TG_GATE_URL}/v1/models" || true)
        echo "${models_code}" > "${TG_ARTIFACT_DIR}/models.status"
        if [[ "${models_code}" == "200" ]]; then
            mark_pass "Text-Generation /v1/models"
        else
            mark_fail "Text-Generation /v1/models expected 200, got ${models_code}"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 && "${TG_GATE_FAST_ENABLE}" == "1" ]]; then
        edge_fast_log="${TG_ARTIFACT_DIR}/test_edge_cases_fast.log"
        edge_fast_summary="${TG_ARTIFACT_DIR}/test_edge_cases_fast_summary.json"
        if timeout "${TG_GATE_FAST_TIMEOUT_SEC}" python3 "${REPO_ROOT}/core-services/text-to-text/test_edge_cases.py" \
            --url "${TG_GATE_URL}" \
            --fast \
            --json-summary "${edge_fast_summary}" \
            > "${edge_fast_log}" 2>&1; then
            if python3 - "${edge_fast_summary}" "${TG_GATE_FAST_EXPECTED_PASS}" \
                > "${TG_ARTIFACT_DIR}/test_edge_cases_fast_validation.txt" <<'PY'
import json
import sys

summary_path = sys.argv[1]
expected = int(sys.argv[2])

with open(summary_path, encoding="utf-8") as fh:
    summary = json.load(fh)

errors = []
passed = int(summary.get("passed", -1))
failed = int(summary.get("failed", -1))
total = int(summary.get("total", -1))
results = summary.get("results", [])

if failed != 0:
    errors.append(f"expected failed=0, got failed={failed}")
if total != expected:
    errors.append(f"expected total={expected}, got total={total}")
if passed != expected:
    errors.append(f"expected passed={expected}, got passed={passed}")

required_ids = ("T01", "T10", "T18", "T19", "T20")
for test_id in required_ids:
    match = None
    for item in results:
        if str(item.get("label", "")).startswith(f"{test_id} "):
            match = item
            break
    if match is None:
        errors.append(f"{test_id} missing from summary results")
        continue
    if str(match.get("tag", "")).upper() != "PASS":
        errors.append(f"{test_id} did not pass (tag={match.get('tag')}, status={match.get('actual_status')})")

if errors:
    print("VALIDATION_FAILED")
    for err in errors:
        print(err)
    raise SystemExit(1)

print("VALIDATION_OK")
PY
            then
                mark_pass "Text-Generation fast edge suite ${TG_GATE_FAST_EXPECTED_PASS}/${TG_GATE_FAST_EXPECTED_PASS}"
            else
                mark_fail "Text-Generation fast edge suite summary validation failed"
                tg_gate_failed=1
            fi
        else
            mark_fail "Text-Generation fast edge suite failed or timed out (${TG_GATE_FAST_TIMEOUT_SEC}s)"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 && "${TG_GATE_PROFILE}" == "full" ]]; then
        edge_log="${TG_ARTIFACT_DIR}/test_edge_cases.log"
        edge_summary="${TG_ARTIFACT_DIR}/test_edge_cases_summary.json"
        if timeout "${TG_GATE_TIMEOUT_SEC}" python3 "${REPO_ROOT}/core-services/text-to-text/test_edge_cases.py" \
            --url "${TG_GATE_URL}" \
            --json-summary "${edge_summary}" \
            > "${edge_log}" 2>&1; then
            if python3 - "${edge_summary}" "${TG_GATE_EXPECTED_PASS}" \
                > "${TG_ARTIFACT_DIR}/test_edge_cases_validation.txt" <<'PY'
import json
import sys

summary_path = sys.argv[1]
expected = int(sys.argv[2])

with open(summary_path, encoding="utf-8") as fh:
    summary = json.load(fh)

errors = []
passed = int(summary.get("passed", -1))
failed = int(summary.get("failed", -1))
total = int(summary.get("total", -1))
results = summary.get("results", [])

if failed != 0:
    errors.append(f"expected failed=0, got failed={failed}")
if total != expected:
    errors.append(f"expected total={expected}, got total={total}")
if passed != expected:
    errors.append(f"expected passed={expected}, got passed={passed}")

required_ids = ("T27", "T29", "T30", "T31", "T32", "T33")
for test_id in required_ids:
    match = None
    for item in results:
        if str(item.get("label", "")).startswith(f"{test_id} "):
            match = item
            break
    if match is None:
        errors.append(f"{test_id} missing from summary results")
        continue
    if str(match.get("tag", "")).upper() != "PASS":
        errors.append(f"{test_id} did not pass (tag={match.get('tag')}, status={match.get('actual_status')})")

if errors:
    print("VALIDATION_FAILED")
    for err in errors:
        print(err)
    raise SystemExit(1)

print("VALIDATION_OK")
PY
            then
                mark_pass "Text-Generation edge suite ${TG_GATE_EXPECTED_PASS}/${TG_GATE_EXPECTED_PASS}"
            else
                mark_fail "Text-Generation edge suite summary validation failed"
                tg_gate_failed=1
            fi
        else
            mark_fail "Text-Generation edge suite failed or timed out (${TG_GATE_TIMEOUT_SEC}s)"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 && "${TG_GATE_PROFILE}" == "full" ]]; then
        internal_models_code=$(curl -sS -m 15 \
            -H "X-Internal-API-Key: ${TG_INTERNAL_API_KEY_FOR_TESTS}" \
            -o "${TG_ARTIFACT_DIR}/internal_models.json" \
            -w "%{http_code}" \
            "${TG_GATE_URL}/v1/internal/models" || true)
        echo "${internal_models_code}" > "${TG_ARTIFACT_DIR}/internal_models.status"
        if [[ "${internal_models_code}" != "200" ]]; then
            mark_fail "GET /v1/internal/models expected 200, got ${internal_models_code}"
            tg_gate_failed=1
        else
            payload_file="${TG_ARTIFACT_DIR}/internal_models_load_payload.json"
            if python3 - "${TG_ARTIFACT_DIR}/internal_models.json" > "${payload_file}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)

payload = None
for item in data.get("data") or []:
    genie_config_path = str(item.get("genie_config_path") or "").strip()
    if genie_config_path:
        payload = {"genie_config_path": genie_config_path}
        break

    model_dir = str(item.get("model_dir") or "").strip()
    if model_dir:
        payload = {"model_dir": model_dir}
        break

active_genie_config_path = str(data.get("active_genie_config_path") or "").strip()
if payload is None and active_genie_config_path:
    payload = {"genie_config_path": active_genie_config_path}

if payload is None:
    raise SystemExit(2)

print(json.dumps(payload))
PY
            then
                internal_load_code=$(curl -sS -m 120 -X POST \
                    -H "Content-Type: application/json" \
                    -H "X-Internal-API-Key: ${TG_INTERNAL_API_KEY_FOR_TESTS}" \
                    -d @"${payload_file}" \
                    -o "${TG_ARTIFACT_DIR}/internal_models_load.json" \
                    -w "%{http_code}" \
                    "${TG_GATE_URL}/v1/internal/models/load" || true)
                echo "${internal_load_code}" > "${TG_ARTIFACT_DIR}/internal_models_load.status"
                if [[ "${internal_load_code}" == "200" ]]; then
                    mark_pass "POST /v1/internal/models/load"
                else
                    mark_fail "POST /v1/internal/models/load expected 200, got ${internal_load_code}"
                    tg_gate_failed=1
                fi
            else
                mark_fail "Failed to derive payload for /v1/internal/models/load"
                tg_gate_failed=1
            fi
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 && "${TG_GATE_PROFILE}" == "full" ]]; then
        concurrency_log="${TG_ARTIFACT_DIR}/concurrency_gate.log"
        concurrency_json="${TG_ARTIFACT_DIR}/concurrency_gate.json"
        if python3 - "${TG_GATE_URL}" "${concurrency_json}" > "${concurrency_log}" 2>&1 <<'PY'
import json
import sys
import threading
import time

try:
    import requests
except Exception as exc:  # pragma: no cover
    print("IMPORT_ERROR", exc)
    raise SystemExit(2)

base = sys.argv[1].rstrip("/")
out_json = sys.argv[2]

payload = {
    "messages": [{"role": "user", "content": "Explain why 2+2=4 in one paragraph."}],
    "max_tokens": 256,
}

results = [{}, {}]

def call(idx: int) -> None:
    start = time.time()
    try:
        resp = requests.post(f"{base}/v1/chat/completions", json=payload, timeout=90)
        duration = time.time() - start
        data = {
            "status": resp.status_code,
            "duration_s": duration,
            "headers": dict(resp.headers),
            "body": resp.text[:400],
        }
        if resp.headers.get("content-type", "").startswith("application/json"):
            try:
                data["json"] = resp.json()
            except Exception:
                pass
        results[idx] = data
    except Exception as exc:
        results[idx] = {"error": str(exc)}

threads = [
    threading.Thread(target=call, args=(0,)),
    threading.Thread(target=call, args=(1,)),
]

threads[0].start()
time.sleep(0.1)
threads[1].start()

for t in threads:
    t.join(timeout=120)

payload_out = {"base": base, "results": results}
with open(out_json, "w", encoding="utf-8") as fh:
    json.dump(payload_out, fh, indent=2)

errors = []
for idx, item in enumerate(results):
    if not item:
        errors.append(f"request[{idx}] missing result")
        continue
    if "error" in item:
        errors.append(f"request[{idx}] error: {item['error']}")
        continue
    status = int(item.get("status", 0) or 0)
    if status not in (200, 503):
        errors.append(f"request[{idx}] unexpected status {status}")
        continue
    if status == 503:
        headers = {k.lower(): v for k, v in (item.get("headers") or {}).items()}
        retry_after = headers.get("retry-after")
        if not retry_after:
            errors.append(f"request[{idx}] 503 missing Retry-After header")
        body = item.get("json") or {}
        code = ((body.get("error") or {}).get("code") if isinstance(body, dict) else None)
        if code != "model_busy":
            errors.append(f"request[{idx}] 503 missing error.code=model_busy")

if errors:
    print("CONCURRENCY_GATE_FAILED")
    for err in errors:
        print(err)
    raise SystemExit(1)

print("CONCURRENCY_GATE_OK")
PY
        then
            mark_pass "Text-Generation concurrency gate"
        else
            mark_fail "Text-Generation concurrency gate failed"
            tg_gate_failed=1
        fi
    fi

    if [[ ${tg_gate_failed} -eq 0 && "${TG_GATE_PROFILE}" == "quick" ]]; then
        mark_pass "quick gate profile: skipped full edge suite and /v1/internal/models/load gate"
    fi

    if [[ ${tg_gate_failed} -ne 0 ]]; then
        collect_tg_diagnostics
    fi
fi

log_section "Text-To-Speech Complexity Gate"
if command -v lizard >/dev/null 2>&1; then
    tts_complexity_log="${TTS_ARTIFACT_DIR}/lizard_tts.log"
    if lizard -E NS -C 10 \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/main.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsApp.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsApp.hpp" \
        > "${tts_complexity_log}" 2>&1; then
        mark_pass "TTS complexity gate (lizard -E NS -C 10)"
    else
        mark_fail "TTS complexity gate failed (see ${tts_complexity_log})"
    fi
else
    mark_warn "lizard not found for TTS complexity gate; skipping complexity enforcement on this runner"
fi

log_section "Text-To-Speech Full-Folder Complexity Report (non-blocking)"
if command -v lizard >/dev/null 2>&1; then
    tts_full_complexity_log="${TTS_ARTIFACT_DIR}/lizard_tts_full_folder.log"
    if lizard -E NS -C 10 \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/main.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsApp.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsApp.hpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsService.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsService.hpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsEngine.cpp" \
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/src/TtsEngine.hpp" \
        > "${tts_full_complexity_log}" 2>&1; then
        mark_pass "TTS full-folder complexity report clean (active + deprecated paths)"
    else
        mark_warn "TTS full-folder complexity threshold exceeded (non-blocking for now; see ${tts_full_complexity_log})"
    fi
else
    mark_warn "lizard not found for TTS full-folder complexity report; skipping"
fi

log_section "Text-To-Speech Contract Gate"
if [[ "${TTS_GATE_ENABLE}" != "1" ]]; then
    echo "[ci] INFO: TTS contract gate disabled (TTS_GATE_ENABLE=${TTS_GATE_ENABLE})"
else
    tts_gate_failed=0

    if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1; then
        mark_fail "docker compose is required for TTS contract gate"
        tts_gate_failed=1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        mark_fail "python3 is required for TTS contract gate"
        tts_gate_failed=1
    fi

    if [[ ${tts_gate_failed} -eq 0 ]]; then
        if (cd "${REPO_ROOT}" && docker compose up -d --no-deps text-to-speech \
            > "${TTS_ARTIFACT_DIR}/compose_up_text_to_speech.log" 2>&1); then
            mark_pass "docker compose up text-to-speech"
        else
            mark_fail "docker compose up text-to-speech failed"
            tts_gate_failed=1
        fi
    fi

    if [[ ${tts_gate_failed} -eq 0 ]]; then
        tts_health_ok=0
        for attempt in $(seq 1 30); do
            tts_health_code=$(curl -sS -m 5 -o "${TTS_ARTIFACT_DIR}/health.json" \
                -w "%{http_code}" "${TTS_GATE_URL}/health" || true)
            echo "${tts_health_code}" > "${TTS_ARTIFACT_DIR}/health.status"
            if [[ "${tts_health_code}" == "200" ]]; then
                tts_health_ok=1
                break
            fi
            sleep 2
        done
        if [[ ${tts_health_ok} -eq 1 ]]; then
            mark_pass "Text-To-Speech /health ready"
        else
            mark_fail "Text-To-Speech /health did not return 200"
            tts_gate_failed=1
        fi
    fi

    if [[ ${tts_gate_failed} -eq 0 ]]; then
        tts_edge_log="${TTS_ARTIFACT_DIR}/test_edge_cases.log"
        tts_edge_summary="${TTS_ARTIFACT_DIR}/test_edge_cases_summary.json"
        if timeout "${TTS_GATE_TIMEOUT_SEC}" python3 "${REPO_ROOT}/core-services/text-to-speech/meloTTS/test_edge_cases.py" \
            --url "${TTS_GATE_URL}" \
            --json-summary "${tts_edge_summary}" \
            > "${tts_edge_log}" 2>&1; then
            if python3 - "${tts_edge_summary}" "${TTS_GATE_EXPECTED_PASS}" \
                > "${TTS_ARTIFACT_DIR}/test_edge_cases_validation.txt" <<'PY'
import json
import sys

summary_path = sys.argv[1]
expected = int(sys.argv[2])

with open(summary_path, encoding="utf-8") as fh:
    summary = json.load(fh)

errors = []
passed = int(summary.get("passed", -1))
failed = int(summary.get("failed", -1))
total = int(summary.get("total", -1))
results = summary.get("results", [])

if failed != 0:
    errors.append(f"expected failed=0, got failed={failed}")
if total != expected:
    errors.append(f"expected total={expected}, got total={total}")
if passed != expected:
    errors.append(f"expected passed={expected}, got passed={passed}")

required_ids = ("T21", "T22", "T23", "T24", "T25", "T26")
for test_id in required_ids:
    match = None
    for item in results:
        if str(item.get("label", "")).startswith(f"{test_id} "):
            match = item
            break
    if match is None:
        errors.append(f"{test_id} missing from summary results")
        continue
    if str(match.get("tag", "")).upper() != "PASS":
        errors.append(f"{test_id} did not pass (tag={match.get('tag')}, status={match.get('actual_status')})")

if errors:
    print("VALIDATION_FAILED")
    for err in errors:
        print(err)
    raise SystemExit(1)

print("VALIDATION_OK")
PY
            then
                mark_pass "Text-To-Speech edge suite ${TTS_GATE_EXPECTED_PASS}/${TTS_GATE_EXPECTED_PASS}"
            else
                mark_fail "Text-To-Speech edge suite summary validation failed"
                tts_gate_failed=1
            fi
        else
            mark_fail "Text-To-Speech edge suite failed or timed out (${TTS_GATE_TIMEOUT_SEC}s)"
            tts_gate_failed=1
        fi
    fi

    if [[ ${tts_gate_failed} -eq 0 ]]; then
        tts_concurrency_log="${TTS_ARTIFACT_DIR}/test_concurrency_contract.log"
        if timeout "${TTS_GATE_TIMEOUT_SEC}" python3 "${REPO_ROOT}/core-services/text-to-speech/meloTTS/test_concurrency_contract.py" \
            --base-url "${TTS_GATE_URL}" \
            > "${tts_concurrency_log}" 2>&1; then
            mark_pass "Text-To-Speech concurrency contract gate (200/429 + Retry-After)"
        else
            mark_fail "Text-To-Speech concurrency contract gate failed (see ${tts_concurrency_log})"
            tts_gate_failed=1
        fi
    fi

    if [[ ${tts_gate_failed} -eq 0 ]]; then
        tts_parity_log="${TTS_ARTIFACT_DIR}/test_golden_parity.log"
        tts_parity_summary="${TTS_ARTIFACT_DIR}/test_golden_parity_summary.json"
        tts_parity_manifest="${REPO_ROOT}/core-services/text-to-speech/meloTTS/tests/golden/tts_parity_prompts.json"
        tts_parity_golden="${REPO_ROOT}/core-services/text-to-speech/meloTTS/tests/golden/tts_parity_expected_sha256.json"
        tts_model_host_dir="${TTS_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-speech/melo-tts-v73/files}"

        if timeout "${TTS_GATE_TIMEOUT_SEC}" python3 "${REPO_ROOT}/core-services/text-to-speech/meloTTS/test_golden_parity.py" \
            --mode verify \
            --base-url "${TTS_GATE_URL}" \
            --manifest "${tts_parity_manifest}" \
            --golden "${tts_parity_golden}" \
            --model-path "${tts_model_host_dir}" \
            --language English \
            --strict-model-checksum \
            --json-summary "${tts_parity_summary}" \
            > "${tts_parity_log}" 2>&1; then
            if python3 - "${tts_parity_summary}" "${TTS_PARITY_EXPECTED_PROMPTS}" \
                > "${TTS_ARTIFACT_DIR}/test_golden_parity_validation.txt" <<'PY'
import json
import sys

summary_path = sys.argv[1]
expected_count = int(sys.argv[2])

with open(summary_path, encoding="utf-8") as fh:
    summary = json.load(fh)

errors = summary.get("errors", [])
actual_hashes = summary.get("actual_hashes", {})

if errors:
    print("PARITY_VALIDATION_FAILED")
    for err in errors:
        print(err)
    raise SystemExit(1)

if len(actual_hashes) != expected_count:
    print("PARITY_VALIDATION_FAILED")
    print(f"expected prompt count {expected_count}, got {len(actual_hashes)}")
    raise SystemExit(1)

print("PARITY_VALIDATION_OK")
PY
            then
                mark_pass "Text-To-Speech golden parity gate ${TTS_PARITY_EXPECTED_PROMPTS}/${TTS_PARITY_EXPECTED_PROMPTS}"
            else
                mark_fail "Text-To-Speech golden parity summary validation failed"
                tts_gate_failed=1
            fi
        else
            mark_fail "Text-To-Speech golden parity gate failed or timed out (${TTS_GATE_TIMEOUT_SEC}s)"
            tts_gate_failed=1
        fi
    fi
fi

log_section "Summary"
if [[ ${failures} -eq 0 ]]; then
    echo "[ci] RESULT: PASS"
    exit 0
fi

echo "[ci] RESULT: FAIL (${failures} check(s) failed)"
exit 1
