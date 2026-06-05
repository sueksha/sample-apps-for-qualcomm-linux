# Test Layout

This repository now has a unified baseline for all core runtime services.
Each core service suite contains 45 cases (23 positive + 22 negative) including edge coverage.

## 1) Canonical Unified Suites

Location:
- `tests/unified/`

Use this for standard diagnostics from host machine to target device:

```bash
python3 tests/unified/run_manifest.py --target-host <TARGET_DEVICE_IP>
```

Host-side expectation:
- services on target must bind to `0.0.0.0` and be reachable via target IP.
- combined HTML report includes service-wise top fail/skip reasons.

Run a single service suite:

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --output tests/unified/reports/ttt_report.json
```

To override the model ID for a suite (Text-To-Text):

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --var model_id=<model_name> \
  --output tests/unified/reports/ttt_report.json
```

Details:
- see [tests/unified/README.md](unified/README.md)

## 2) Cross-Service E2E Suites

Location:
- `core-services/orchestrator/tools/`

Examples:
```bash
I2T_BASE_URL=http://127.0.0.1:8080 ORCH_BASE_URL=http://127.0.0.1:8090 \
python3 core-services/orchestrator/tools/i2t_orchestrator_extensive_suite.py

I2T_BASE_URL=http://127.0.0.1:8080 ORCH_BASE_URL=http://127.0.0.1:8090 \
python3 core-services/orchestrator/tools/i2t_orchestrator_additional_68_suite.py
```
