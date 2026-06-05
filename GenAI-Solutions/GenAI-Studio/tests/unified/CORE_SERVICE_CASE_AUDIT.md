# Core-Service Test Case Audit

This audit maps existing service-level suites to the unified suites in `tests/unified/suites`.

Audit date: 2026-05-07 (repo-local)

## 1) Existing suite inventory (canonical)

| Service | Existing suite path(s) | Existing profile |
|---|---|---|
| text-to-text | `tests/unified/suites/text-to-text.yaml` | positive + negative + edge (large prompt + storage lifecycle) |
| speech-to-text | `tests/unified/suites/speech-to-text.yaml` | positive + negative + edge (stream/realtime + long audio) |
| image-to-text | `tests/unified/suites/image-to-text.yaml` | direct + orchestrator responses, semantic checks, large prompt, multi-turn |
| text-to-image | `tests/unified/suites/text-to-image.yaml` | generation/edits/variations, auth, large prompt, parameter edge checks |
| text-to-speech | `tests/unified/suites/text-to-speech.yaml` | synthesis + legacy routes, large prompt, payload edge checks |
| orchestrator | `tests/unified/suites/orchestrator.yaml`, `core-services/orchestrator/tools/i2t_orchestrator_extensive_suite.py`, `core-services/orchestrator/tools/i2t_orchestrator_additional_68_suite.py` | proxy contracts + cross-service E2E |

## 2) Unified baseline mapping

| Service | Unified suite path | Unified count |
|---|---|---|
| text-to-text | `tests/unified/suites/text-to-text.yaml` | 45 |
| speech-to-text | `tests/unified/suites/speech-to-text.yaml` | 45 |
| image-to-text | `tests/unified/suites/image-to-text.yaml` | 45 |
| text-to-image | `tests/unified/suites/text-to-image.yaml` | 45 |
| text-to-speech | `tests/unified/suites/text-to-speech.yaml` | 45 |
| orchestrator | `tests/unified/suites/orchestrator.yaml` | 45 |

## 3) Standard runner

- Single suite:
  - `python3 tests/unified/run_http_suite.py --suite <suite.yaml> --target-host <device-ip> --output <report.json>`
- All suites:
  - `python3 tests/unified/run_manifest.py --target-host <device-ip>`

Outputs are generated as JSON + HTML for both per-service and combined reports.
