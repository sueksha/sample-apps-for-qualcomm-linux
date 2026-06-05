#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# Extensive I2T + Orchestrator endpoint validation suite.
#
# Coverage:
#   - Direct Image-To-Text service (:8080)
#   - Orchestrator API gateway (:8090)
#   - Cross-service orchestrator proxies (TG/STT/ImageGen/OpenAI routes)
#
# Test count: 70 cases
#
# Usage:
#   python3 core-services/orchestrator/tools/i2t_orchestrator_extensive_suite.py
#
# Optional env:
#   I2T_BASE_URL=http://127.0.0.1:8080
#   ORCH_BASE_URL=http://127.0.0.1:8090
#   I2T_ORCH_TEST_TIMEOUT=300
#   I2T_ORCH_TEST_WORKDIR=/tmp/i2t_orch_extensive_suite
# ---------------------------------------------------------------------

from __future__ import annotations

import json
import os
import struct
import time
import uuid
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Callable, Dict, List, Optional, Set, Tuple, Union

import requests
from PIL import Image, ImageDraw


I2T_BASE_URL = os.getenv("I2T_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ORCH_BASE_URL = os.getenv("ORCH_BASE_URL", "http://127.0.0.1:8090").rstrip("/")
TIMEOUT = float(os.getenv("I2T_ORCH_TEST_TIMEOUT", "300"))
WORKDIR = Path(os.getenv("I2T_ORCH_TEST_WORKDIR", "/tmp/i2t_orch_extensive_suite"))


@dataclass
class Result:
    status_code: int
    headers: Dict[str, str]
    text: str
    json_body: Optional[dict]
    elapsed_ms: float
    extra: Dict[str, object]


class Suite:
    def __init__(self) -> None:
        self.total = 0
        self.passed = 0
        self.failed = 0
        self.results: List[dict] = []
        self.ctx: Dict[str, object] = {}

    def log(self, msg: str) -> None:
        print(msg, flush=True)

    def run_case(
        self,
        case_id: int,
        name: str,
        expected: Union[int, Set[int], Tuple[int, ...]],
        call: Callable[[], Result],
        check: Optional[Callable[[Result], Tuple[bool, str]]] = None,
    ) -> None:
        self.total += 1
        ok = False
        details = ""
        status_code = -1
        elapsed_ms = 0.0
        text_snippet = ""
        try:
            result = call()
            status_code = result.status_code
            elapsed_ms = result.elapsed_ms
            text_snippet = (result.text or "").replace("\n", " ")[:240]
            expected_set = {expected} if isinstance(expected, int) else set(expected)
            status_ok = status_code in expected_set
            check_ok = True
            check_msg = "ok"
            if check is not None:
                check_ok, check_msg = check(result)
            ok = status_ok and check_ok
            if not status_ok:
                details = f"expected {sorted(expected_set)}, got {status_code}"
            elif not check_ok:
                details = check_msg
            else:
                details = check_msg
        except Exception as exc:  # noqa: BLE001
            ok = False
            details = f"exception: {type(exc).__name__}: {exc}"

        if ok:
            self.passed += 1
            self.log(f"PASS T{case_id:02d} [{status_code}] {name} ({elapsed_ms:.1f} ms)")
        else:
            self.failed += 1
            self.log(f"FAIL T{case_id:02d} [{status_code}] {name} ({elapsed_ms:.1f} ms)")
            self.log(f"     ↳ {details}")
            if text_snippet:
                self.log(f"     ↳ body: {text_snippet}")

        self.results.append(
            {
                "case_id": case_id,
                "name": name,
                "ok": ok,
                "status_code": status_code,
                "expected": expected if isinstance(expected, int) else sorted(list(set(expected))),
                "elapsed_ms": elapsed_ms,
                "details": details,
                "body_snippet": text_snippet,
            }
        )


def to_result(resp: requests.Response, elapsed_ms: float, extra: Optional[dict] = None) -> Result:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001
        body = None
    return Result(
        status_code=resp.status_code,
        headers={k: v for k, v in resp.headers.items()},
        text=resp.text,
        json_body=body if isinstance(body, dict) else None,
        elapsed_ms=round(elapsed_ms, 3),
        extra=extra or {},
    )


def http_get(url: str, timeout: float = 30.0) -> Result:
    t0 = time.perf_counter()
    resp = requests.get(url, timeout=timeout)
    return to_result(resp, (time.perf_counter() - t0) * 1000.0)


def http_delete(url: str, timeout: float = 30.0) -> Result:
    t0 = time.perf_counter()
    resp = requests.delete(url, timeout=timeout)
    return to_result(resp, (time.perf_counter() - t0) * 1000.0)


def http_post_json(url: str, payload: dict, timeout: float = TIMEOUT) -> Result:
    t0 = time.perf_counter()
    resp = requests.post(url, json=payload, timeout=timeout)
    return to_result(resp, (time.perf_counter() - t0) * 1000.0)


def http_post_raw_json(url: str, raw_body: str, timeout: float = TIMEOUT) -> Result:
    t0 = time.perf_counter()
    resp = requests.post(
        url,
        data=raw_body.encode("utf-8"),
        headers={"Content-Type": "application/json"},
        timeout=timeout,
    )
    return to_result(resp, (time.perf_counter() - t0) * 1000.0)


def http_post_multipart(
    url: str,
    *,
    files: Optional[dict] = None,
    data: Optional[dict] = None,
    timeout: float = TIMEOUT,
) -> Result:
    t0 = time.perf_counter()
    resp = requests.post(url, files=files or {}, data=data or {}, timeout=timeout)
    return to_result(resp, (time.perf_counter() - t0) * 1000.0)


def http_post_json_stream(url: str, payload: dict, timeout: float = TIMEOUT) -> Result:
    t0 = time.perf_counter()
    resp = requests.post(url, json=payload, stream=True, timeout=timeout)
    lines: List[str] = []
    done_seen = False
    session_id = ""
    for raw in resp.iter_lines(decode_unicode=True):
        if raw is None:
            continue
        line = str(raw).strip()
        if not line:
            continue
        lines.append(line)
        if not line.startswith("data:"):
            continue
        payload_line = line[5:].strip()
        if payload_line == "[DONE]":
            done_seen = True
            break
        try:
            obj = json.loads(payload_line)
            if isinstance(obj, dict) and obj.get("type") == "session":
                sid = str(obj.get("session_id", "")).strip()
                if sid:
                    session_id = sid
        except Exception:
            pass

    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    return Result(
        status_code=resp.status_code,
        headers={k: v for k, v in resp.headers.items()},
        text="\n".join(lines),
        json_body=None,
        elapsed_ms=round(elapsed_ms, 3),
        extra={"done_seen": done_seen, "session_id": session_id, "lines": len(lines)},
    )


def make_test_image_bytes(seed: int = 1) -> bytes:
    width, height = 512, 342
    img = Image.new("RGB", (width, height), (240, 240, 240))
    draw = ImageDraw.Draw(img)
    draw.rectangle([20 + seed, 20, 220, 170], fill=(210, 90, 60))
    draw.ellipse([260, 80, 420, 250], fill=(70, 180, 110))
    draw.polygon([(380, 300), (460, 220), (500, 300)], fill=(90, 120, 220))
    draw.text((20, 300), f"seed={seed}", fill=(20, 20, 20))
    buffer = BytesIO()
    img.save(buffer, format="JPEG", quality=95)
    return buffer.getvalue()


def make_wav_bytes(duration_s: float = 0.5, sample_rate: int = 16000) -> bytes:
    num_samples = int(sample_rate * duration_s)
    num_channels = 1
    bits_per_sample = 16
    byte_rate = sample_rate * num_channels * bits_per_sample // 8
    block_align = num_channels * bits_per_sample // 8
    data_size = num_samples * block_align
    chunk_size = 36 + data_size
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        chunk_size,
        b"WAVE",
        b"fmt ",
        16,
        1,
        num_channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        data_size,
    )
    return header + b"\x00" * data_size


def check_json_has(path: List[str]) -> Callable[[Result], Tuple[bool, str]]:
    def _check(result: Result) -> Tuple[bool, str]:
        obj = result.json_body
        if not isinstance(obj, dict):
            return False, "response is not JSON object"
        cur: object = obj
        for key in path:
            if not isinstance(cur, dict) or key not in cur:
                return False, f"missing path: {'/'.join(path)}"
            cur = cur[key]
        return True, "ok"

    return _check


def check_non_empty_text_choice(result: Result) -> Tuple[bool, str]:
    body = result.json_body
    if not isinstance(body, dict):
        return False, "response is not JSON object"
    choices = body.get("choices")
    if not isinstance(choices, list) or not choices:
        return False, "missing choices[]"
    msg = choices[0].get("message", {}) if isinstance(choices[0], dict) else {}
    content = msg.get("content", "") if isinstance(msg, dict) else ""
    if not isinstance(content, str) or not content.strip():
        return False, "empty assistant content"
    return True, "ok"


def check_sse_done(result: Result) -> Tuple[bool, str]:
    if result.status_code != 200:
        return False, "non-200 stream response"
    done_seen = bool(result.extra.get("done_seen")) or ("[DONE]" in (result.text or ""))
    if not done_seen:
        return False, "missing [DONE]"
    return True, "ok"


def check_sse_error_and_done(result: Result) -> Tuple[bool, str]:
    if result.status_code != 200:
        return False, "non-200 stream response"
    txt = result.text or ""
    if "\"error\"" not in txt and "\"code\": \"upstream_error\"" not in txt:
        return False, "missing SSE error event"
    done_seen = bool(result.extra.get("done_seen")) or ("[DONE]" in txt)
    if not done_seen:
        return False, "missing [DONE]"
    return True, "ok"


def check_sse_done_and_session(result: Result) -> Tuple[bool, str]:
    if result.status_code != 200:
        return False, "non-200 stream response"
    if not result.extra.get("done_seen"):
        return False, "missing [DONE]"
    sid = str(result.extra.get("session_id", "")).strip()
    if not sid:
        return False, "missing session_id event"
    return True, "ok"


def check_sse_done_and_store_session(ctx_key: str) -> Callable[[Result], Tuple[bool, str]]:
    def _check(result: Result) -> Tuple[bool, str]:
        ok, msg = check_sse_done_and_session(result)
        if not ok:
            return ok, msg
        sid = str(result.extra.get("session_id", "")).strip()
        suite.ctx[ctx_key] = sid
        return True, "ok"

    return _check


def check_json_has_and_store(path: List[str], ctx_key: str) -> Callable[[Result], Tuple[bool, str]]:
    def _check(result: Result) -> Tuple[bool, str]:
        obj = result.json_body
        if not isinstance(obj, dict):
            return False, "response is not JSON object"
        cur: object = obj
        for key in path:
            if not isinstance(cur, dict) or key not in cur:
                return False, f"missing path: {'/'.join(path)}"
            cur = cur[key]
        val = str(cur).strip()
        if not val:
            return False, f"path {'/'.join(path)} is empty"
        suite.ctx[ctx_key] = val
        return True, "ok"

    return _check


def check_status_services(result: Result) -> Tuple[bool, str]:
    body = result.json_body
    if not isinstance(body, dict):
        return False, "response is not JSON object"
    services = body.get("services")
    if not isinstance(services, list) or len(services) < 4:
        return False, "services list missing or too short"
    names = {str(item.get("name", "")) for item in services if isinstance(item, dict)}
    required = {"Text-Generation", "Speech-To-Text", "Image-Generation", "Image-To-Text"}
    if not required.issubset(names):
        return False, "status missing one or more services"
    return True, "ok"


def check_models_list(result: Result) -> Tuple[bool, str]:
    body = result.json_body
    if not isinstance(body, dict):
        return False, "response is not JSON object"
    data = body.get("data")
    if not isinstance(data, list) or not data:
        return False, "missing non-empty data[]"
    return True, "ok"


def check_preprocess_path_and_store(ctx_key: str) -> Callable[[Result], Tuple[bool, str]]:
    def _check(result: Result) -> Tuple[bool, str]:
        body = result.json_body
        if not isinstance(body, dict):
            return False, "response is not JSON object"
        path = body.get("pixel_values_path")
        if not isinstance(path, str) or not path.strip():
            return False, "missing pixel_values_path"
        suite.ctx[ctx_key] = path.strip()
        return True, "ok"

    return _check


def check_model_found(model_id_key: str) -> Callable[[Result], Tuple[bool, str]]:
    def _check(result: Result) -> Tuple[bool, str]:
        body = result.json_body
        if not isinstance(body, dict):
            return False, "response is not JSON object"
        data = body.get("data")
        if not isinstance(data, list) or not data:
            return False, "missing model list"
        model_id = str(data[0].get("id", "")).strip() if isinstance(data[0], dict) else ""
        if not model_id:
            return False, "first model has no id"
        suite.ctx[model_id_key] = model_id
        return True, "ok"

    return _check


suite = Suite()


def main() -> int:
    WORKDIR.mkdir(parents=True, exist_ok=True)
    img1 = make_test_image_bytes(seed=1)
    img2 = make_test_image_bytes(seed=2)
    wav = make_wav_bytes()
    (WORKDIR / "img1.jpg").write_bytes(img1)
    (WORKDIR / "img2.jpg").write_bytes(img2)
    (WORKDIR / "sample.wav").write_bytes(wav)

    suite.ctx["session_direct"] = f"s-{uuid.uuid4().hex[:12]}"

    suite.log("=" * 60)
    suite.log("I2T + Orchestrator Extensive Suite")
    suite.log(f"I2T base : {I2T_BASE_URL}")
    suite.log(f"Orch base: {ORCH_BASE_URL}")
    suite.log(f"Workdir  : {WORKDIR}")
    suite.log("=" * 60)

    i2t = I2T_BASE_URL
    orch = ORCH_BASE_URL

    suite.run_case(1, "I2T GET /health", 200, lambda: http_get(f"{i2t}/health"), check_json_has(["status"]))
    suite.run_case(2, "I2T GET /v1/models", 200, lambda: http_get(f"{i2t}/v1/models"), check_models_list)
    suite.run_case(3, "I2T capture first model id", 200, lambda: http_get(f"{i2t}/v1/models"), check_model_found("i2t_model_id"))
    suite.run_case(4, "I2T GET unknown -> 404", 404, lambda: http_get(f"{i2t}/does-not-exist"))
    suite.run_case(5, "I2T GET /v1/preprocess wrong method", {404, 405}, lambda: http_get(f"{i2t}/v1/preprocess"))
    suite.run_case(6, "I2T POST /v1/preprocess missing file -> 400", 400, lambda: http_post_multipart(f"{i2t}/v1/preprocess", files={}))
    suite.run_case(
        7,
        "I2T POST /v1/preprocess empty file -> 400",
        400,
        lambda: http_post_multipart(
            f"{i2t}/v1/preprocess",
            files={"file": ("empty.jpg", b"", "image/jpeg")},
        ),
    )
    suite.run_case(
        8,
        "I2T POST /v1/preprocess disabled server path -> 501",
        501,
        lambda: http_post_multipart(
            f"{i2t}/v1/preprocess",
            files={"file": ("img1.jpg", img1, "image/jpeg")},
        ),
    )
    suite.run_case(9, "I2T POST /v1/chat/completions invalid JSON -> 400", 400, lambda: http_post_raw_json(f"{i2t}/v1/chat/completions", "{bad-json"))
    suite.run_case(10, "I2T chat missing messages -> 400", 400, lambda: http_post_json(f"{i2t}/v1/chat/completions", {"model": "local"}))
    suite.run_case(11, "I2T chat messages wrong type -> 400", 400, lambda: http_post_json(f"{i2t}/v1/chat/completions", {"messages": "hello"}))
    suite.run_case(12, "I2T chat no user text -> 400", 400, lambda: http_post_json(f"{i2t}/v1/chat/completions", {"messages": [{"role": "assistant", "content": "hi"}]}))
    suite.run_case(
        13,
        "I2T text chat non-stream",
        200,
        lambda: http_post_json(
            f"{i2t}/v1/chat/completions",
            {"messages": [{"role": "user", "content": "Reply with one short word: hello"}], "stream": False, "max_tokens": 24},
        ),
        check_non_empty_text_choice,
    )
    suite.run_case(
        14,
        "I2T text chat stream",
        200,
        lambda: http_post_json_stream(
            f"{i2t}/v1/chat/completions",
            {"messages": [{"role": "user", "content": "Say hello in one short line"}], "stream": True, "max_tokens": 32},
        ),
        check_sse_done,
    )
    suite.run_case(
        15,
        "I2T chat invalid pixel_values_path -> 400",
        400,
        lambda: http_post_json(
            f"{i2t}/v1/chat/completions",
            {
                "messages": [{"role": "user", "content": "Describe image"}],
                "pixel_values_path": "/tmp/not-found/pixel_values.raw",
                "stream": False,
            },
        ),
    )
    suite.run_case(16, "I2T vision invalid JSON -> 400", 400, lambda: http_post_raw_json(f"{i2t}/v1/vision/chat/completions", "{bad-json"))
    suite.run_case(17, "I2T vision missing messages -> 400", 400, lambda: http_post_json(f"{i2t}/v1/vision/chat/completions", {"pixel_values_path": "/tmp/x"}))
    suite.run_case(18, "I2T vision missing pixel path -> 400", 400, lambda: http_post_json(f"{i2t}/v1/vision/chat/completions", {"messages": [{"role": "user", "content": "Describe"}]}))
    suite.run_case(
        19,
        "I2T vision bad pixel path -> 400",
        400,
        lambda: http_post_json(
            f"{i2t}/v1/vision/chat/completions",
            {"messages": [{"role": "user", "content": "Describe"}], "pixel_values_path": "/tmp/not-found/path.raw"},
        ),
    )

    suite.run_case(20, "Orch GET /api/status", 200, lambda: http_get(f"{orch}/api/status"), check_status_services)
    suite.run_case(21, "Orch GET /api/i2t/models", 200, lambda: http_get(f"{orch}/api/i2t/models"), check_models_list)
    suite.run_case(
        22,
        "Orch preprocess missing file -> 422/400",
        {422, 400},
        lambda: http_post_multipart(f"{orch}/api/i2t/preprocess", files={}),
    )
    suite.run_case(
        23,
        "Orch preprocess valid image",
        200,
        lambda: http_post_multipart(
            f"{orch}/api/i2t/preprocess",
            files={"file": ("img1.jpg", img1, "image/jpeg")},
            data={"prompt": "Describe this image"},
        ),
        check_preprocess_path_and_store("orch_pixel_path"),
    )
    suite.run_case(24, "Orch i2t/vision invalid JSON -> 400", 400, lambda: http_post_raw_json(f"{orch}/api/i2t/vision", "{bad-json"))
    suite.run_case(25, "Orch i2t/vision missing messages -> 400", 400, lambda: http_post_json(f"{orch}/api/i2t/vision", {"pixel_values_path": suite.ctx.get("orch_pixel_path", "")}))
    suite.run_case(26, "Orch i2t/vision missing pixel path -> 400", 400, lambda: http_post_json(f"{orch}/api/i2t/vision", {"messages": [{"role": "user", "content": "Describe"}]}))
    suite.run_case(
        27,
        "Orch i2t/vision bad pixel path -> stream error event",
        200,
        lambda: http_post_json(
            f"{orch}/api/i2t/vision",
            {"messages": [{"role": "user", "content": "Describe"}], "pixel_values_path": "/tmp/not-found/pixel.raw"},
        ),
        check_sse_error_and_done,
    )
    suite.run_case(
        28,
        "Orch i2t/vision valid stream with session",
        200,
        lambda: http_post_json_stream(
            f"{orch}/api/i2t/vision",
            {
                "messages": [{"role": "user", "content": "Describe this image in one concise sentence."}],
                "pixel_values_path": str(suite.ctx.get("orch_pixel_path", "")),
                "stream": True,
                "max_tokens": 64,
            },
        ),
        check_sse_done_and_store_session("orch_session_id"),
    )

    if not suite.ctx.get("orch_session_id"):
        suite.ctx["orch_session_id"] = f"s-{uuid.uuid4().hex[:12]}"

    suite.run_case(
        29,
        "Orch i2t/chat missing session_id -> 400",
        400,
        lambda: http_post_json(
            f"{orch}/api/i2t/chat",
            {"messages": [{"role": "user", "content": "Hello"}], "stream": False},
        ),
    )
    suite.run_case(
        30,
        "Orch i2t/chat missing messages -> 400",
        400,
        lambda: http_post_json(
            f"{orch}/api/i2t/chat",
            {"session_id": str(suite.ctx.get("orch_session_id", "")), "stream": False},
        ),
    )
    suite.run_case(
        31,
        "Orch i2t/chat valid non-stream",
        200,
        lambda: http_post_json(
            f"{orch}/api/i2t/chat",
            {
                "session_id": str(suite.ctx.get("orch_session_id", "")),
                "messages": [{"role": "user", "content": "Continue with one short sentence."}],
                "stream": False,
                "max_tokens": 48,
            },
        ),
        check_non_empty_text_choice,
    )
    suite.run_case(
        32,
        "Orch i2t/chat valid stream",
        200,
        lambda: http_post_json_stream(
            f"{orch}/api/i2t/chat",
            {
                "session_id": str(suite.ctx.get("orch_session_id", "")),
                "messages": [{"role": "user", "content": "Give one short continuation."}],
                "stream": True,
                "max_tokens": 48,
            },
        ),
        check_sse_done,
    )
    suite.run_case(33, "Orch i2t/reset no session", 200, lambda: http_post_json(f"{orch}/api/i2t/reset", {}))
    suite.run_case(
        34,
        "Orch i2t/reset with session_id",
        200,
        lambda: http_post_json(f"{orch}/api/i2t/reset", {"session_id": str(suite.ctx.get("orch_session_id", ""))}),
    )

    suite.run_case(35, "Orch GET /api/tg/models", 200, lambda: http_get(f"{orch}/api/tg/models"), check_models_list)
    suite.run_case(36, "Orch TG chat invalid JSON -> 400", 400, lambda: http_post_raw_json(f"{orch}/api/tg/chat", "{bad-json"))
    suite.run_case(37, "Orch TG chat missing messages -> 400", 400, lambda: http_post_json(f"{orch}/api/tg/chat", {"model": "genie"}))
    suite.run_case(
        38,
        "Orch TG chat valid non-stream",
        200,
        lambda: http_post_json(
            f"{orch}/api/tg/chat",
            {"messages": [{"role": "user", "content": "Reply only with YES"}], "stream": False, "max_tokens": 16},
        ),
        check_non_empty_text_choice,
    )
    suite.run_case(
        39,
        "Orch TG chat valid stream",
        200,
        lambda: http_post_json_stream(
            f"{orch}/api/tg/chat",
            {"messages": [{"role": "user", "content": "Reply with one short greeting"}], "stream": True, "max_tokens": 24},
        ),
        check_sse_done,
    )
    suite.run_case(40, "Orch TG reset", 200, lambda: http_post_json(f"{orch}/api/tg/reset", {}))

    suite.run_case(41, "Orch GET /api/stt/models", 200, lambda: http_get(f"{orch}/api/stt/models"), check_models_list)
    suite.run_case(42, "Orch GET /api/stt/languages", 200, lambda: http_get(f"{orch}/api/stt/languages"))
    suite.run_case(
        43,
        "Orch STT transcribe missing file -> 422/400",
        {422, 400},
        lambda: http_post_multipart(f"{orch}/api/stt/transcribe", files={}),
    )
    suite.run_case(
        44,
        "Orch STT transcribe short wav",
        200,
        lambda: http_post_multipart(
            f"{orch}/api/stt/transcribe",
            files={"file": ("sample.wav", wav, "audio/wav")},
            data={"response_format": "json", "task": "transcribe"},
        ),
    )

    suite.run_case(45, "Orch GET /api/img/models", 200, lambda: http_get(f"{orch}/api/img/models"), check_models_list)
    suite.run_case(46, "Orch GET /api/img/params", 200, lambda: http_get(f"{orch}/api/img/params"))
    suite.run_case(47, "Orch /api/img/generate missing prompt -> 400", 400, lambda: http_post_json(f"{orch}/api/img/generate", {"model": "stable-diffusion-2-1"}))
    suite.run_case(48, "Orch /v1/images/generations missing prompt -> 400", 400, lambda: http_post_json(f"{orch}/v1/images/generations", {"model": "stable-diffusion-2-1"}))
    suite.run_case(
        49,
        "Orch /v1/images/edits missing image -> 400",
        400,
        lambda: http_post_multipart(
            f"{orch}/v1/images/edits",
            files={},
            data={"prompt": "edit this image"},
        ),
    )
    suite.run_case(50, "Orch /v1/images/variations missing image -> 400", 400, lambda: http_post_multipart(f"{orch}/v1/images/variations", files={}))
    suite.run_case(51, "Orch /v1/images/files missing id -> 404", 404, lambda: http_get(f"{orch}/v1/images/files/"))
    suite.run_case(52, "Orch /v1/images/files unknown id -> 404/400", {404, 400}, lambda: http_get(f"{orch}/v1/images/files/not-a-real-image-id"))

    suite.run_case(53, "OpenAI GET /v1/models", 200, lambda: http_get(f"{orch}/v1/models"), check_models_list)
    suite.run_case(54, "OpenAI GET /v1/models/nonexistent -> 404", 404, lambda: http_get(f"{orch}/v1/models/nonexistent-model-id"))
    suite.run_case(55, "OpenAI chat invalid JSON -> 400", 400, lambda: http_post_raw_json(f"{orch}/v1/chat/completions", "{bad-json"))
    suite.run_case(
        56,
        "OpenAI chat text-only non-stream",
        200,
        lambda: http_post_json(
            f"{orch}/v1/chat/completions",
            {"messages": [{"role": "user", "content": "Answer with one short word: ping"}], "stream": False, "max_tokens": 24},
        ),
        check_non_empty_text_choice,
    )
    suite.run_case(
        57,
        "OpenAI chat text-only stream",
        200,
        lambda: http_post_json_stream(
            f"{orch}/v1/chat/completions",
            {"messages": [{"role": "user", "content": "Say a short hello"}], "stream": True, "max_tokens": 24},
        ),
        check_sse_done,
    )
    suite.run_case(
        58,
        "OpenAI chat vision via pixel_values_path",
        200,
        lambda: http_post_json(
            f"{orch}/v1/chat/completions",
            {
                "messages": [{"role": "user", "content": "Describe this image in one sentence."}],
                "pixel_values_path": str(suite.ctx.get("orch_pixel_path", "")),
                "stream": False,
                "max_tokens": 64,
            },
        ),
        check_non_empty_text_choice,
    )
    suite.run_case(
        59,
        "OpenAI audio transcriptions missing file -> 400",
        400,
        lambda: http_post_multipart(f"{orch}/v1/audio/transcriptions", files={}),
    )
    suite.run_case(
        60,
        "OpenAI audio transcriptions short wav",
        200,
        lambda: http_post_multipart(
            f"{orch}/v1/audio/transcriptions",
            files={"file": ("sample.wav", wav, "audio/wav")},
            data={"model": "whisper-tiny", "response_format": "json"},
        ),
    )
    suite.run_case(
        61,
        "OpenAI audio translations short wav",
        200,
        lambda: http_post_multipart(
            f"{orch}/v1/audio/translations",
            files={"file": ("sample.wav", wav, "audio/wav")},
            data={"model": "whisper-tiny", "response_format": "json"},
        ),
    )

    suite.run_case(62, "OpenAI GET /v1/realtime", 200, lambda: http_get(f"{orch}/v1/realtime"))
    suite.run_case(
        63,
        "OpenAI realtime create session",
        200,
        lambda: http_post_json(
            f"{orch}/v1/realtime/sessions",
            {"model": "gpt-4o-mini-transcribe", "language": "en", "task": "transcribe"},
        ),
        check_json_has_and_store(["id"], "rt_session_id"),
    )

    if not suite.ctx.get("rt_session_id"):
        suite.ctx["rt_session_id"] = str(uuid.uuid4())

    suite.run_case(64, "OpenAI realtime get session", {200, 404}, lambda: http_get(f"{orch}/v1/realtime/sessions/{suite.ctx['rt_session_id']}"))
    suite.run_case(
        65,
        "OpenAI realtime append empty audio -> 400/422",
        {400, 422},
        lambda: to_result(
            requests.post(
                f"{orch}/v1/realtime/sessions/{suite.ctx['rt_session_id']}/audio",
                data=b"",
                headers={"Content-Type": "application/octet-stream"},
                timeout=TIMEOUT,
            ),
            0.0,
        ),
    )
    suite.run_case(66, "OpenAI realtime finalize", {200, 400, 404}, lambda: http_post_json(f"{orch}/v1/realtime/sessions/{suite.ctx['rt_session_id']}/finalize", {}))
    suite.run_case(67, "OpenAI realtime delete session", {200, 404}, lambda: http_delete(f"{orch}/v1/realtime/sessions/{suite.ctx['rt_session_id']}"))
    suite.run_case(68, "OpenAI realtime get deleted session -> 404", 404, lambda: http_get(f"{orch}/v1/realtime/sessions/{suite.ctx['rt_session_id']}"))

    suite.run_case(
        69,
        "Orch root HTML page",
        200,
        lambda: http_get(f"{orch}/"),
        lambda r: ("text/html" in str(r.headers.get("content-type", "")).lower(), "ok" if "text/html" in str(r.headers.get("content-type", "")).lower() else "content-type is not text/html"),
    )
    suite.run_case(
        70,
        "Direct I2T vision non-stream using orch pixel path",
        200,
        lambda: http_post_json(
            f"{i2t}/v1/vision/chat/completions",
            {
                "messages": [{"role": "user", "content": "Describe this image briefly."}],
                "pixel_values_path": str(suite.ctx.get("orch_pixel_path", "")),
                "stream": False,
                "max_tokens": 64,
            },
        ),
        check_non_empty_text_choice,
    )

    report = {
        "total": suite.total,
        "passed": suite.passed,
        "failed": suite.failed,
        "i2t_base_url": I2T_BASE_URL,
        "orch_base_url": ORCH_BASE_URL,
        "results": suite.results,
        "captured": {
            "i2t_model_id": suite.ctx.get("i2t_model_id", ""),
            "orch_pixel_path": suite.ctx.get("orch_pixel_path", ""),
            "orch_session_id": suite.ctx.get("orch_session_id", ""),
            "rt_session_id": suite.ctx.get("rt_session_id", ""),
        },
    }

    ts = int(time.time())
    report_path = WORKDIR / f"i2t_orchestrator_extensive_report_{ts}.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    suite.log("")
    suite.log("=" * 60)
    suite.log(f"Functional tests: {suite.passed}/{suite.total} passed, {suite.failed} failed")
    suite.log("=" * 60)
    suite.log(f"Report: {report_path}")

    return 0 if suite.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
