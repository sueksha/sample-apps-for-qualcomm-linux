# Text-To-Speech Model Generation and Placement (MeloTTS)

TTS model generation for this service is done using the **Voice-AI-TTS-1.1.1.0** notebook flow, then the generated `.qnn` bundle is placed in the runtime model directory.

## Document Control

- Owner: GenAI Studio Maintainers
- Last reviewed: 2026-04-29

## Host vs Target Boundary

- Run notebook generation and any `qpm-cli` install on a host or staging machine.
- Copy generated `.qnn` artifacts and the `melo_sdk/` build payload onto the target before running `docker build`.
- Canonical target SDK staging path is `core-services/text-to-speech/meloTTS/melo_sdk`.

## 1) Model generation source

- QPM notebook/tool reference: `https://qpm.qualcomm.com/#/main/tools/details/VoiceAI_TTS`
- SDK/runtime baseline used in this repo:
  - Melo SDK package: `VoiceAI_TTS 1.1.1.0`
  - QAIRT runtime: `2.45.x` (recommended)
- Ensure you are using the following versions of python dependencies:
  - torch==1.13.1
  - torchaudio==0.13.1
  - onnx==1.16.1
  - onnxruntime==1.18.1
  - onnxscript==0.7.0
  - onnxsim==0.6.2

If you use QPM3 to obtain the SDK source, run this on the host machine only:

```bash
qpm-cli --install VoiceAI_TTS -v 1.1.1.0 --path /opt/qcom/qpm/VoiceAI_TTS/1.1.1.0 --silent

TARGET_REPO=/path/to/genai-studio-on-target
rsync -av /opt/qcom/qpm/VoiceAI_TTS/1.1.1.0/melo_sdk/ \
  ubuntu@<target-host>:${TARGET_REPO}/core-services/text-to-speech/meloTTS/melo_sdk/
```


Run your Melo-TTS notebook flow for the target profile **(V73/QCS9075 class)** and export the packaged `.qnn` model.

Validated sample-app model naming pattern from the QPM package:

- `melo_en.64_bit.qnn_v2.33.0.qnn` (English)
- optionally language variants such as `melo_es...` or `melo_zh...`

Note:

- Use the QPM sample-app model naming convention when validating against `melo_sdk` on LE/QCS9075.
- If a notebook-exported artifact fails, verify runtime wiring first before regenerating the model.

## 2) Place on device (run on target device)

Compose default host model directory:

- `/opt/genai-studio-models/text-to-speech/melo-tts-v73/files`

Place generated `.qnn` file(s) in that directory.

The service can accept either:

- direct file path to a `.qnn` file, or
- directory path containing `.qnn` files

## 3) SDK source and runtime libraries

`libtts.so` is built inside the TTS Docker image from source during `docker build`.

Source selection:

- QPM3 install root on host: `/opt/qcom/qpm/VoiceAI_TTS/1.1.1.0/melo_sdk`
- Canonical target staging copy in repo: `core-services/text-to-speech/meloTTS/melo_sdk`
- Optional bundled archive on target: `core-services/text-to-speech/meloTTS/1.1.1.0.zip`

Artifacts copied into runtime image:

- `/usr/lib/libtts.so`
- `/usr/lib/rfsa/adsp/libtts_impl_skel.so`

The host-side `/usr/lib/libtts.so` is not required for the container.
Only QAIRT/DSP/CDSP runtime mounts remain host-provided from compose.

## 4) Verify before build/run (run on target device)

```bash
MODEL_DIR=/opt/genai-studio-models/text-to-speech/melo-tts-v73/files
ls -lah "$MODEL_DIR"
ls "$MODEL_DIR"/*.qnn
ls "$MODEL_DIR"/libtts_impl_skel.so \
   "$MODEL_DIR"/libQnnSystem.so \
   "$MODEL_DIR"/libQnnHtpV73.so \
   "$MODEL_DIR"/libQnnHtpV73Skel.so
```

If you keep models in a different host location, set compose override before `docker compose up`:

```bash
export TTS_MODEL_HOST_DIR=/your/custom/path/to/melo-tts-v73/files
```

## 5) Runtime compatibility note (QCS9075)

Preferred target is QAIRT `2.45.x` with a matching Melo model.
However, if TTS startup fails with:

- `tts_impl_init error=-2147482611`
- `TTSEngine::init() failed`

then validate with a `v2.44.0` Melo model as a fallback on the same QAIRT `2.45.x` runtime.

The fallback directory should contain `melo_en.64_bit.qnn_v2.44.0.qnn` at the top level of `MODEL_DIR`.
