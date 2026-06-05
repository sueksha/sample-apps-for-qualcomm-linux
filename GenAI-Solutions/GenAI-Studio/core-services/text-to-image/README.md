# Image-Generation Service (SD2.1)

Image-Generation backend runs on `:8084`.
Client traffic can use orchestrator `:8090` or direct `:8084` (debug/advanced).

Canonical build/run/rebuild commands live in repo-root `README.md`:

1. `One Bring-Up Path`
2. `Strict clean rebuild cycle`
3. `Six-service functional tests`

## Prerequisites (Target)

Required model directory:

- `/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075`

Required runtime libs:

- `/opt/qairt/current/qairt_245_flat_libs`

Required model files:

- `text_encoder.bin` or `text_encoder*.bin`
- `unet.bin` or `unet*.bin`
- `vae.bin` or `vae*.bin`
- `tokenizer/vocab.json`
- `tokenizer/merges.txt`

Required runtime mounts are handled by `docker-compose.yml` when host paths are valid (`HOST_RPC_LIB_DIR`, `/usr/lib/dsp`, `/usr/lib/rfsa/adsp`, `/usr/share/qcom`).

## Build & Start Service

```bash
DOCKER_BUILDKIT=1 docker build --progress=plain -t text-to-image:latest core-services/text-to-image/
```

Export required environment variables and start the service:

```bash
export IMG_QAIRT_LIBS_HOST_DIR=/opt/qairt/current/qairt_245_flat_libs
export IMAGEGEN_MODEL_DIR=/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075
export HF_CACHE_HOST_DIR=/opt/genai-studio-cache/huggingface
mkdir -p "$HF_CACHE_HOST_DIR"

docker compose up -d text-to-image
```

## Validate

Backend health:

```bash
curl -sS http://localhost:8084/health
```

Expected response shape:

```json
{"status":"ok"}
```

Orchestrator path (recommended):

```bash
curl -sS -X POST "http://localhost:8090/v1/images/generations" \
  -H "Content-Type: application/json" \
  --data-raw '{"model":"stable-diffusion-2-1","prompt":"a mountain landscape at sunrise","size":"512x512","response_format":"b64_json"}'
```

Expected response shape:

```json
{
  "created": 1712345678,
  "data": [{"b64_json": "..."}]
}
```

Header probe for arbitration observability:

```bash
curl -sS -D - -o /tmp/imagegen_orch.json -X POST "http://localhost:8090/v1/images/generations" \
  -H "Content-Type: application/json" \
  --data-raw '{"model":"stable-diffusion-2-1","prompt":"header probe","size":"512x512"}' \
  | grep -Ei 'HTTP/|X-Npu-Wait-Ms|Retry-After'
```

## Routing Policy

- canonical client path: orchestrator `POST /v1/images/generations` on `:8090`
- backend `:8084` is the internal plumbing and debug path
- mixed I2T + ImageGen loads require arbitration

Reference:

- `docs/NPU_ARBITRATION_RUNBOOK.md`

## Direct I2T + T2I Coexistence (No Orchestrator)

When running both services directly on target (`:8080` + `:8084`), keep these compose env defaults:

- `IMAGEGEN_I2T_RELEASE_URL=http://127.0.0.1:8080/v1/session/release?unload=1`
- `IMAGEGEN_I2T_RELEASE_BEFORE_GENERATE=0`
- `IMAGEGEN_TRANSIENT_RETRY_ATTEMPTS=48`
- `IMAGEGEN_TRANSIENT_RETRY_BACKOFF_MS=1500`

Behavior:

- on transient `rc=256`, T2I retries and requests I2T unload through `/v1/session/release?unload=1`
- I2T rejects unload with `503 upstream_busy` while I2T inference is in-flight, avoiding request races
- once I2T is idle, unload succeeds and T2I retry proceeds

## Troubleshooting `rc=256`

1. Prefer orchestrator route (`:8090`) for default client flow.
2. For direct mode (`:8084`), ensure the four arbitration envs above are set.
3. Confirm `HOST_RPC_LIB_DIR` resolves correctly and compose mounts succeed.
4. Ensure required `/dev/fastrpc-*` devices are available on target.
5. Reduce NPU contention (temporarily stop other NPU-heavy services).
6. Inspect logs:

```bash
docker logs --tail 200 text-to-image
docker exec text-to-image sh -lc 'last=$(ls -t /tmp/sd21_server_*.log 2>/dev/null | head -n1); echo "$last"; [ -n "$last" ] && sed -n "1,260p" "$last"'
```

Expected error response shape (top failure):

```json
{
  "error": {
    "type": "server_error",
    "message": "Direct runner command failed with rc=256"
  }
}
```

## Advanced Debug Path (Direct `docker run`)

Direct `docker run` is debug-only. Use it only when compose wiring itself is under suspicion.
Keep the canonical onboarding path compose-first.

## Endpoints

- `GET /health`
- `GET /v1/models`
- `GET /v1/models/{id}`
- `GET /v1/images/generations/params`
- `POST /v1/images/generations`
- `POST /generate` (legacy)

## Advanced References

- `core-services/text-to-image/MODEL_SETUP.md`
- `docs/API_CONTRACTS.md`
- `docs/TROUBLESHOOTING_GUIDE.md`
- `docs/NPU_ARBITRATION_RUNBOOK.md`
