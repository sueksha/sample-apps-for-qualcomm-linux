# Orchestrator Code Flow

This document explains request routing and transformation logic in `core-services/orchestrator/device_app.py`.

## 1) App Bootstrap

- FastAPI app created with CORS enabled
- async `httpx` clients initialized per traffic type
- startup hook only checks I2T readiness (no orchestrator-side preprocessing)
- middleware attaches `X-Orchestrator-Time-Ms` on every response

## 2) Core Routing Responsibilities

The orchestrator serves two API planes:

1. **UI plane (`/api/*`)**
   - richer service-specific helpers
   - additional timing envelopes
2. **Unified OpenAI plane (`/v1/*`)**
   - proxy/normalize traffic toward edge backends

## 3) Text Path (`/api/tg/*` and `/v1/chat/completions`)

### 3.1 `/api/tg/chat`

Flow:

1. parse and validate request JSON
2. resolve upstream target (edge vs cloud) via headers/env
3. forward request to upstream `/v1/chat/completions`
4. if streaming, proxy SSE bytes
5. if non-streaming, add orchestrator timing payload

### 3.2 `/v1/chat/completions`

- Text-only chat route to Text-Generation backend.
- If payload indicates I2T/vision intent (`messages[].content[].image_url`, `session_id`, `pixel_values_path`),
  orchestrator returns `400` and instructs clients to use `POST /v1/responses`.

## 4) Speech Path (`/api/stt/*` and `/v1/audio/*`)

- `/api/stt/transcribe`
  - receives multipart upload
  - forwards to STT upstream transcription/translation endpoint
  - merges upstream timing headers into response body
- `/v1/audio/transcriptions` and `/v1/audio/translations`
  - raw proxy pass-through with preserved multipart payload

## 5) Image Generation Path (`/api/img/*` and `/v1/images/*`)

### 5.1 Request normalization

- incoming model aliases are normalized to edge-supported `stable-diffusion-2-1`

### 5.2 `/api/img/generate`

Flow:

1. read raw body
2. normalize model field
3. acquire shared NPU arbitration lock (`npu_arbiter.acquire_npu_slot`)
4. send to image backend `/v1/images/generations`
5. optional fallback arbitration logic (`IMG_I2T_ARBITRATION_ENABLED=1`):
   - can stop/restart Image-To-Text container when image backend returns specific shared-resource failures
6. if lock wait exceeds timeout, return `429` with `npu_busy`
7. rewrites returned file URLs to orchestrator-hosted `/v1/images/files/{id}`
8. attaches orchestrator + upstream timings and `X-Npu-Wait-Ms` response header

### 5.3 `/v1/images/*`

- generations route uses normalized proxy path
- edits/variations/files routes are forwarded to image backend

## 6) Image-To-Text Path

### 6.1 Canonical inference route

- `POST /v1/responses` on orchestrator forwards directly to I2T backend `POST /v1/responses`.
- Legacy wrappers `/api/i2t/vision` and `/api/i2t/chat` are removed.

### 6.2 Session reset helper

- `/api/i2t/reset`
  - forwards reset request to I2T backend `/v1/session/reset`
  - keeps explicit `session_id` behavior via payload/header

## 7) Model Aggregation (`/v1/models`)

- fetches model lists from TG, I2T, STT, and ImageGen
- merges and de-duplicates by model id
- returns unified OpenAI-style model list

## 8) Proxy Utility Layer

Key helpers:

- `_proxy_raw_request` — forwards raw body + headers + query params
- `_proxy_json_post` — forwards JSON and preserves streamed responses
- `_response_from_upstream` — copies response headers/content with stream support
- `npu_arbiter.acquire_npu_slot` — serializes I2T/ImageGen on shared accelerator

## 9) Timing Instrumentation

Timing helpers annotate multiple stages:

- parse/read/setup durations
- upstream roundtrip durations
- backend preprocessing duration (reported from I2T `x_timing.image_preprocess_ms`)
- backend timing headers merged into response body where available

## 10) Failure Handling

- standardized OpenAI-style error body (`_error_body`)
- upstream connectivity failures converted to `503`
- NPU lock timeout converted to `429` (`npu_busy`)
- authentication failures in cloud mode return `401`
- non-JSON upstream responses are passed through as raw response
