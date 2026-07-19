# Interactive Bot — ESP32-S3 Voice Assistant

A miniature interactive robot: ESP32-S3-N16R8 handles audio capture/playback,
wake-word-gated conversation sessions, and a 3-display animated face. A
Python bridge on a laptop handles the AI pipeline (STT/Chat/TTS via OpenAI
Realtime API, or fallback chained OpenAI/Gemini pipelines).

## ⚠️ Before you clone/push — security

- **`config.h` is git-ignored** (see `.gitignore`) because it contains your
  real WiFi password in plaintext. Copy `bot_firmware/bot_firmware/config.h.example`
  to `config.h` and fill in your actual values — never commit `config.h` itself.
- **API keys are never stored in any file** — both `bridge.py` and the OpenAI/Gemini
  SDKs read them from environment variables (`OPENAI_API_KEY`, `GEMINI_API_KEY`) set
  in your terminal session, not hardcoded anywhere. Verify this stays true before
  every commit — search for `sk-` or `AIza` prefixes before pushing if you're ever
  unsure.
- If you ever accidentally commit a real secret, treat that key/password as
  compromised — rotate it (generate a new one, revoke the old one) rather than
  just deleting it from a later commit, since it remains in git history otherwise.

## Repo structure

```
bot_firmware/
  bot_firmware/
    bot_firmware.ino     — main sketch, state machine
    config.h.example      — TEMPLATE, copy to config.h and fill in real values
    config.h               — (git-ignored) your real settings, create this locally
    mic.h                   — I2S mic capture + on-device VAD
    speaker.h               — I2S speaker playback
    face.h                   — 3-display animated face (2 eyes + mouth)
    button.h                 — debounced push button
    network.h                — WiFi/Serial transport, typed message protocol
  WIRING.md                  — pin assignments, wiring guide, troubleshooting

bot_bridge/
  bridge.py                  — PC-side AI pipeline bridge (3 provider options)
  BRIDGE_SETUP.md             — setup instructions, dependencies, API keys

diagnostics/
  i2c_scanner/                — standalone I2C address scanner
  wifi_scanner/                — standalone WiFi network scanner
  wifi_connect_diag/            — standalone WiFi connection diagnostic
```

## Quick start

1. Copy `bot_firmware/bot_firmware/config.h.example` to `config.h`, fill in
   your WiFi SSID/password and laptop's LAN IP.
2. Open `bot_firmware.ino` in Arduino IDE (all files must be in the same
   folder, named `bot_firmware`). Install required libraries — see WIRING.md.
3. Set your API key as an environment variable, then run `bridge.py` — see
   BRIDGE_SETUP.md.
4. Flash the ESP32, power it on, talk to it.

Full wiring diagram, pin assignments, and troubleshooting steps are in
`WIRING.md`. Bridge/API setup details are in `BRIDGE_SETUP.md`.
