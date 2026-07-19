# PC Bridge Setup

# PC Bridge Setup

This is the Python script that runs on your laptop during the expo. It handles TWO kinds
of requests from the ESP32:
- **Wake-word checks** — short 2-second chunks sent continuously while the bot is idle, asking "was the name said?" (fast Whisper-only check, no GPT/TTS)
- **Conversation turns** — full utterances during an active session, run through Whisper → GPT → TTS

## 1. Install dependencies

```bash
pip install openai pydub pyserial
```

**Also install ffmpeg** (needed by `pydub` to decode the TTS MP3 output into raw PCM
for the ESP32 to play). This is the one non-Python dependency — install it BEFORE the
expo, not during:

- **Windows:** Download from https://www.gyan.dev/ffmpeg/builds/ (get the "essentials" build), unzip, add the `bin` folder to your PATH. Test with `ffmpeg -version` in a new terminal.
- **Mac:** `brew install ffmpeg`
- **Linux:** `sudo apt install ffmpeg`

## 2. Set the bot's name

`BOT_NAME` near the top of `bridge.py` must exactly match `BOT_NAME` in the ESP32's
`config.h` (case doesn't matter, spelling does). This is the wake word.

## 3. Set your API key

**Windows PowerShell:**
```powershell
$env:OPENAI_API_KEY="sk-your-actual-key-here"
```

**Windows cmd:**
```cmd
set OPENAI_API_KEY=sk-your-actual-key-here
```

**Mac/Linux:**
```bash
export OPENAI_API_KEY="sk-your-actual-key-here"
```

Do this in the SAME terminal window you'll run `bridge.py` from — the env var doesn't
persist across terminal sessions unless you add it to your shell profile.

## 4. Find your laptop's LAN IP (for WiFi mode)

**Windows:** `ipconfig` → look for "IPv4 Address" under your active WiFi adapter (something like `192.168.1.XX`)
**Mac/Linux:** `ifconfig` or `ip addr` → look for `inet` under your WiFi interface

Put that IP into `PC_SERVER_IP` in the ESP32's `config.h`, then re-upload the firmware.

**Important:** Your laptop and the ESP32 must be on the SAME WiFi network. Expo/conference
WiFi sometimes isolates devices from each other (client isolation) for security — if the
ESP32 can't reach your laptop despite correct IP/firewall settings, this is likely why.
Backup plan: use your phone's mobile hotspot for both laptop and ESP32 instead of expo WiFi.

## 5. Firewall

Windows Firewall may block incoming connections to port 8765 the first time you run this.
When prompted, click "Allow access" (for Private networks at minimum).

## 6. Run it

**WiFi mode (default):**
```bash
python bridge.py
```
You should see:
```
[BOOT] Starting bridge...
[BOOT] OpenAI pipeline ready. Wake word: 'Aria'. Model: gpt-4o-mini, STT: whisper-1, TTS: tts-1
[SERVER] Listening on 0.0.0.0:8765 — waiting for ESP32 to connect...
```

**USB Serial mode (fallback if WiFi is unreliable):**
```bash
python bridge.py --mode serial --port COM5
```
(Replace `COM5` with the actual port — check Device Manager, same port you use to upload
firmware. Also flip `TRANSPORT_MODE` to `TRANSPORT_SERIAL` in config.h and re-upload first.)

## 7. What you'll see

**While idle (bot listening for wake word), each time it hears a sound:**
```
[14:30:01] [RECV] WAKE_CHECK: 64000 bytes
[14:30:02] [WAKE] heard: 'hey there'  -> no match
```

**Once the wake word is heard, then during an active conversation:**
```
[14:30:15] [RECV] WAKE_CHECK: 64000 bytes
[14:30:16] [WAKE] heard: 'hey Aria'  -> MATCH
[14:30:22] [RECV] CONVERSATION: 96000 bytes
[14:30:23] [STT] 'What can you tell me about robotics?'  (0.84s)
[14:30:24] [GPT] 'Robotics combines mechanical engineering, electronics...'  (1.12s)
[14:30:25] [TTS] 48000 bytes PCM generated  (0.95s)
[14:30:25] [PIPELINE] Total round-trip: 2.91s
[14:30:25] [SEND] Sending 48000 bytes audio reply
```

If something fails, the log tells you exactly which stage broke (WAKE/STT/GPT/TTS/network) —
read the last `[ERROR]` line, that's your problem.

## Wake-word false positives/negatives — this is normal, don't panic

Because wake-word checking runs on short 2-second chunks through Whisper (not a purpose-
built wake-word model), it won't be perfect:
- Occasionally it'll start a session on a false positive (someone says a similar-sounding
  word). Harmless — the session just times out on silence after `SESSION_SILENCE_TIMEOUT_MS`
  if nobody actually talks to it.
- Occasionally it'll miss the name being said. Just say it again, or press the button —
  the button always works as a guaranteed manual override regardless of wake-word state.

## Before the expo — test this end to end at least once

Don't let the first real test be in front of people. Run through one full conversation at
your desk, both directions, at least twice, so you've seen what "working" actually looks
and sounds like and can instantly recognize "broken."

## Cost note

`gpt-4o-mini` + `whisper-1` + `tts-1` is a deliberately cheap/fast combination for live
demo responsiveness. A typical short exchange costs well under a cent. Don't worry about
API costs during testing/demo — this combination is not expensive even with heavy use.
