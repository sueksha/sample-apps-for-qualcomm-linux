# NPU Arbitration Runbook (Image-To-Text + Image-Generation)

This runbook defines the operational contract for mixed Image-To-Text (I2T) and Image-Generation (ImageGen) usage on shared accelerator targets.

## 1) Problem Signature

Typical failure pattern when workloads overlap on the same NPU:

- ImageGen API error: `Direct runner command failed with rc=256`
- Runner log: `QNN deviceCreate failed`
- DSP transport errors: `14001`, `Unknown rpc status 0x000003ff`

These are accelerator contention symptoms, not model file corruption by default.

## 2) Runtime Contract

On this device class, I2T and ImageGen are treated as accelerator-exclusive workloads.

Contract:

1. Route image generation through orchestrator (`:8090`), not direct ImageGen (`:8084`), for mixed workload operation.
2. Orchestrator serializes I2T/ImageGen execution with a shared NPU lock.
3. If the lock wait exceeds timeout, orchestrator returns `429` (`npu_busy`) with `Retry-After`.
4. Legacy fallback (`IMG_I2T_ARBITRATION_ENABLED=1`) may stop/restart I2T on `rc=256` retry path.

## 3) Required Environment

Set on orchestrator container:

- `NPU_ARB_ENABLED=1`
- `NPU_ARB_TIMEOUT_SEC=180`
- `IMG_I2T_ARBITRATION_ENABLED=1`

These are already set in `docker-compose.yml` for the orchestrator service.

## 4) API Usage Policy

Preferred production path:

- `POST http://localhost:8090/v1/images/generations`

Direct service path (`:8084`) is backend-internal and not supported for client traffic, because it bypasses orchestrator arbitration.

## 5) Quick Verification

With full stack up (including `image-to-text`), run:

```bash
curl -sS -X POST "http://localhost:8090/v1/images/generations" \
  -H "Content-Type: application/json" \
  --data-raw '{"model":"stable-diffusion-2-1","prompt":"npu arbitration check","size":"512x512","response_format":"url","seed":1234,"steps":20}'
```

Expected:

- HTTP `200`
- valid `data[0].url` or `b64_json`
- response header `X-Npu-Wait-Ms` present

Optional header probe:

```bash
curl -sS -D - -o /tmp/img_resp.json -X POST "http://localhost:8090/v1/images/generations" \
  -H "Content-Type: application/json" \
  --data-raw '{"model":"stable-diffusion-2-1","prompt":"npu wait header probe","size":"512x512","response_format":"url","seed":1235,"steps":20}' \
  | grep -Ei 'HTTP/|X-Npu-Wait-Ms|Retry-After'
```

## 6) Incident Response

If image generation fails:

1. Confirm you are calling `:8090` (not `:8084`) in mixed workload mode.
2. Check orchestrator env:
   - `NPU_ARB_ENABLED=1`
   - `IMG_I2T_ARBITRATION_ENABLED=1`
3. Inspect orchestrator logs:
   - wait timing and arbitration messages
4. Inspect `text-to-image` logs and latest `/tmp/sd21_server_*.log`.
5. If still unstable, temporarily serialize manually:
   - stop `image-to-text`
   - run text-to-image request
   - restart `image-to-text`

## 7) Known Limitation

Orchestrator lock applies to orchestrator-routed traffic only.
Any client that calls service containers directly bypasses arbitration and is outside supported production behavior.

## 8) Ownership Checklist

Before release, ensure:

1. UI and SDK clients target `:8090` routes.
2. Mixed I2T + ImageGen regression test passes.
3. `rc=256` troubleshooting section points to this runbook.
4. SRE docs include `429 npu_busy` retry behavior.
