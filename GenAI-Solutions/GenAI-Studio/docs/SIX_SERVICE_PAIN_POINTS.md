# Six-Service Pain Points (Observed Audit)

> This is a Historical audit report. For active troubleshooting, see [TROUBLESHOOTING_GUIDE.md](TROUBLESHOOTING_GUIDE.md).

Audit date: 2026-05-10  
Target used: `ubuntu@10.92.174.17`  
Method: strict dependency check -> rebuild -> compose up -> six service tests.

## Service Result Summary

| Service | Result | What Worked | What Failed |
|---|---|---|---|
| Text-To-Text | PASS | `/health`, `/v1/chat/completions` | None in smoke run |
| Image-To-Text | PASS | `/health`, `/v1/responses` with image+text | None in smoke run |
| Text-To-Image | FAIL | `/health` | generation returned `500` with `rc=256` |
| Speech-To-Text | PASS with adjustment | `/health`, transcription with valid model id | docs default model id mismatch on this target |
| Text-To-Speech | PASS | `/health`, `/v1/audio/speech` wav output | None in smoke run |
| Orchestrator | PASS (chat path) | `/health`, `/api/status`, `/v1/chat/completions` | image generation path inherited T2I backend failure |

## Cross-Service Pain Points

1. Host RPC library trap:
`/usr/lib/libcdsprpc.so*` and related names can be directories on some target images.
If compose mounts from `/usr/lib`, runtime fails.  
Required fix: `HOST_RPC_LIB_DIR=/usr/lib/aarch64-linux-gnu` in `.env`.

2. Repo drift between host copy and target copy:
the target `docker-compose.yml` did not match the current canonical repo version.
This caused missing env/mount behavior and service inconsistencies.

3. Test/document path drift:
legacy docs referenced removed service-local test paths.
Canonical path is now `tests/unified/` (single source), including T2I cases.

4. Model-id drift:
STT docs and many scripts assume `whisper-tiny`, but this target returned
`Unsupported model 'whisper-tiny'` and reported `model: whisper-1` in `/health`.

## Service-Wise Pain Points and Fix Ideas

## 1) Text-To-Text

Pain points:

- none in this audit run.

Hardening ideas:

- keep functional smoke in CI (`/v1/chat/completions`) after each target rebuild.

## 2) Image-To-Text

Pain points:

- strict single-active-session behavior can return `409 session_conflict` if a
  previous `X-Session-Id` is still active.
- large `data:` image payloads can fail preprocessing command launch.

Hardening ideas:

- add an explicit pre-test reset step (`POST /api/i2t/reset`) in all smoke docs.
- prefer `https://` image URLs for large assets.
- add explicit limit/error text in service docs and test harness.

## 3) Text-To-Image

Pain points:

- direct generation returned:
  `{"error":{"message":"Direct runner command failed with rc=256","type":"server_error"}}`
- backend requires auth (`Authorization: Bearer qti-demo-key`) for direct `:8084`.
- target compose lacked newer orchestration knobs used for mixed I2T/ImageGen arbitration.

Observed log clue:

- orchestrator log printed:
  `rc=256 detected; I2T arbitration is disabled (IMG_I2T_ARBITRATION_ENABLED=0)`

Hardening ideas:

- sync target compose with canonical repo compose before bring-up.
- ensure arbitration envs are present when mixed I2T + ImageGen loads are expected.
- keep direct `:8084` as debug path; prefer orchestrator `:8090`.

## 4) Speech-To-Text

Pain points:

- docs/test examples with `model=whisper-tiny` failed on this target build.

What worked:

- using `model=whisper-1` succeeded for `/v1/audio/transcriptions`.

Hardening ideas:

- do not hard-code STT model id in smoke docs.
- first query `/health` or `/v1/models`, then use returned canonical model id.

## 5) Text-To-Speech

Pain points:

- env variables are present but path composition is hard to reason about
  (duplicate segments and mixed delimiters in runtime logs).

Hardening ideas:

- normalize path construction in entrypoint/run script.
- keep one canonical `LD_LIBRARY_PATH` and one canonical `ADSP_LIBRARY_PATH` template.

## 6) Orchestrator

Pain points:

- chat proxy path worked, but image generation route failed when backend returned `rc=256`.
- behavior depends on environment flags present in target compose, so repo drift is high risk.

Hardening ideas:

- include a startup self-check endpoint that reports critical arbitration flags.
- fail fast at startup when required flags are missing for mixed workloads.

## Required Documentation Guardrails

1. Put host vs target tags before every command block.
2. Add a preflight check that rejects stale/legacy target compose files.
3. Use model-id autodetection for STT test commands.
4. Include one mandatory functional test per service (not only `/health`).
5. Keep a single canonical six-service runbook and link it from root README.

---

## See Also

- [TROUBLESHOOTING_GUIDE.md](TROUBLESHOOTING_GUIDE.md) — Complete troubleshooting reference with error signatures, 5-step playbook, and triage paths
- [README.md](../README.md) — Full-stack bring-up instructions
- [setup/DEVICE_SETUP.md](setup/DEVICE_SETUP.md) — Device provisioning guide
