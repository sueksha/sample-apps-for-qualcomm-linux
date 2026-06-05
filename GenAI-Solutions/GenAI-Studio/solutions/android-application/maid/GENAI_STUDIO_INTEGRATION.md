# Maid + GenAI Studio Integration Guide

End-to-end guide: clone → build → configure → run Maid against Qualcomm GenAI
Studio endpoints.

Tested on: Maid `main`, GenAI Studio v2, Android device / emulator.

---

## How It Works

Maid supports multiple remote LLM providers. The **Open AI** provider uses the
official `openai` npm package with a fully configurable `baseURL` and `apiKey`
stored in the app's settings. Because GenAI Studio exposes an OpenAI-compatible
API through its orchestrator, **no code changes are needed** — you only need to
set the base URL and API key in the app's UI after installing it.

The integration path inside the app:

```
Maid UI (chat screen)
  └── LanguageModelProvider (context/language-model/index.tsx)
        └── OpenAIProvider (context/language-model/open-ai.tsx)
              └── openai npm package
                    └── POST /v1/chat/completions  (streaming SSE)
                          └── GenAI Studio Orchestrator :8090
                                └── Text2Text container :8088 (LLaMA 3.2-3B)
```

**What works with GenAI Studio:**

| Feature | Status | Notes |
|---------|--------|-------|
| Chat (streaming) | ✅ Works | SSE streaming, `[DONE]` terminator — fully compatible |
| Model list | ✅ Works | `/v1/models` returns all GenAI Studio models |
| Custom base URL | ✅ Works | Editable in app settings UI |
| Custom API key | ✅ Works | Any non-empty string accepted |
| Custom headers | ✅ Works | Editable in app settings UI |
| Image input (vision) | ⚠️ Not tested | `imagesSupported: false` in OpenAI provider |
| TTS / STT | ℹ️ Separate | Handled by companion app Maise, not this provider |

---

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| Node.js ≥ 18 + Yarn | `node --version`, `yarn --version` |
| Android SDK + ADB | For building and deploying to device |
| Expo CLI | Installed via `yarn install` |
| Android device or emulator | Connected via USB or running locally |
| GenAI Studio v2 on QLI target | All containers healthy before testing |
| Network access from Android device to QLI target | Device and QLI on same network |

---

## Step 1 — Clone the Repository

```bash
git clone https://github.com/Mobile-Artificial-Intelligence/maid.git
cd maid
```

---

## Step 2 — Install Dependencies

```bash
yarn install
```

---

## Step 3 — Verify GenAI Studio is Running

From the machine where you will build (or from the Android device's network):

```bash
GENAI_IP=<GENAI_DEVICE_IP>   # e.g. 10.92.165.205

curl http://$GENAI_IP:8090/api/status
curl http://$GENAI_IP:8090/v1/models
```

`/api/status` should show all services as `"status":"ok"`.
`/v1/models` should list `llama3.2-3B`, `whisper-tiny`, `melo-tts-English`, etc.

Verify streaming chat works (this is exactly what the app sends):

```bash
curl -X POST http://$GENAI_IP:8090/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer sk-dummy' \
  -d '{"model":"genie","messages":[{"role":"user","content":"Say hello."}],"stream":true,"max_tokens":30}'
```

Expected: SSE stream of `data: {...}` chunks ending with `data: [DONE]`.

---

## Step 4 — Run the Tests

```bash
yarn test
```

The test suite covers account, login, register, and app-level flows. It does
not test the OpenAI provider directly (no network calls in tests), but confirms
the app scaffolding is intact before building.

---

## Step 5 — Build and Install on Android

### Debug build (recommended for first run)

```bash
yarn android
```

This builds a debug APK and installs it on the connected device/emulator via
ADB. Requires a connected Android device with USB debugging enabled, or a
running emulator.

### Release APK

```bash
yarn build-android
```

The APK is output to `android/app/build/outputs/apk/release/app-release.apk`.
Install it manually:

```bash
adb install android/app/build/outputs/apk/release/app-release.apk
```

---

## Step 6 — Configure the App to Use GenAI Studio

Once the app is installed and running on the device:

1. Open the app and tap the **settings / model** icon (bottom navigation).
2. Under **Provider**, select **Open AI**.
3. Set the following fields:

| Field | Value |
|-------|-------|
| **Base URL** | `http://<GENAI_DEVICE_IP>:8090/v1` |
| **API Key** | `sk-dummy` (any non-empty string) |
| **Model** | `genie` |

> **Important:** The Android device must be on the same network as the QLI
> target. If the device is on Wi-Fi and the QLI target is on a wired LAN,
> ensure routing between them is configured.

> **`genie` model:** This is a virtual alias handled by the orchestrator that
> routes to the active LLM backend (`llama3.2-3B`). It does not appear in the
> `/v1/models` list — that is expected. You can also use `llama3.2-3B` directly
> if you want to bypass the orchestrator queue.

4. Tap **Save** / navigate back to the chat screen.
5. The model selector should populate with the models returned by
   `GET /v1/models` from GenAI Studio.

---

## Step 7 — Test the Integration

### From the app UI

Send a message in the chat screen. You should see the response stream in
token-by-token as the LLM generates it.

### Via curl (same request the app sends)

```bash
# Streaming chat — exactly what the app's OpenAIProvider sends
curl -X POST http://<GENAI_DEVICE_IP>:8090/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer sk-dummy' \
  -d '{
    "model": "genie",
    "messages": [
      {"role": "user", "content": "Say hello in one sentence."}
    ],
    "stream": true,
    "max_tokens": 50
  }'
```

Expected output:

```
data: {"choices":[{"delta":{"content":"","role":"assistant"},...}],...}
data: {"choices":[{"delta":{"content":"Hello"},...}],...}
data: {"choices":[{"delta":{"content":"!"},...}],...}
data: {"choices":[{"delta":{},"finish_reason":"stop",...}],...}
data: [DONE]
```

---

## Troubleshooting

**App shows "OpenAI API key not set" warning in logs**
The API key field is empty. Set any non-empty string (e.g. `sk-dummy`) in the
app settings under Open AI → API Key.

**Model list is empty after setting base URL**
The app fetches models when the OpenAI instance is created (on `apiKey` or
`baseURL` change). If the list stays empty:
- Confirm `curl http://<GENAI_IP>:8090/v1/models` works from the same network
- Check that the Android device can reach the QLI target IP
- Verify the base URL ends with `/v1` (no trailing slash needed — the app
  passes it directly to the `openai` npm package which handles path joining)

**Chat sends but no response / connection timeout**
- The QLI target may not be reachable from the Android device's network
- Run `adb shell curl http://<GENAI_IP>:8090/health` to test connectivity
  from the device itself

**`genie` model not in the model picker dropdown**
`genie` is a virtual orchestrator alias — it is intentionally absent from
`/v1/models`. Type it manually in the model field or use `llama3.2-3B`.

**App crashes on startup**
Run `adb logcat -s ReactNativeJS` to see JS errors. Most startup crashes are
caused by missing native modules — ensure `yarn install` completed successfully
and the build was done after installing dependencies.

---

## How the OpenAI Provider Works (Code Reference)

The relevant file is `context/language-model/open-ai.tsx`.

**Initialization** — runs whenever `apiKey`, `baseURL`, or `headers` change:
```ts
const openaiInstance = new OpenAI({
  apiKey,
  baseURL,                    // set to http://<GENAI_IP>:8090/v1
  defaultHeaders: headers,
  fetch: expoFetch as typeof fetch,   // uses Expo's fetch for React Native
});
```

**Model list fetch** — runs after the OpenAI instance is created:
```ts
const response = await openai.models.list();   // GET /v1/models
setModels(response.data.map((model) => model.id));
```

**Chat (streaming)** — called on every user message:
```ts
const stream = await openai.chat.completions.create({
  model,          // "genie" or "llama3.2-3B"
  messages,       // full conversation history
  stream: true,   // SSE streaming
  ...parameters,  // temperature, top_p, etc. from settings
});

for await (const event of stream) {
  const chunk = event.choices[0]?.delta?.content;
  if (chunk) onUpdate(chunk);   // updates the UI token by token
}
```

All three calls are fully compatible with GenAI Studio's orchestrator.

---

## Quick Reference

```bash
# Clone and install
git clone https://github.com/Mobile-Artificial-Intelligence/maid.git
cd maid && yarn install

# Verify GenAI Studio
curl http://<GENAI_IP>:8090/api/status
curl http://<GENAI_IP>:8090/v1/models

# Build and install debug APK
yarn android

# In the app: Settings → Provider: Open AI
#   Base URL : http://<GENAI_IP>:8090/v1
#   API Key  : sk-dummy
#   Model    : genie
```

---

## Port Reference

| Port | Service | Use for |
|------|---------|---------|
| `8090` | Orchestrator | All client traffic — use this |
| `8088` | Text-Generation direct | Debug only |
| `8083` | TTS direct | Debug only |
| `8081` | STT direct | Debug only |

---

## Changes Made to the Upstream Repo

**None.** The app works with GenAI Studio out of the box via the existing
Open AI provider. The only file added is this guide.
