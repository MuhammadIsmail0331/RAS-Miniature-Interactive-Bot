# Interactive Bot — Wiring & Setup Guide

## STEP 0 — Before wiring anything: verify I2C addresses

Your right eye and mouth display SHARE one I2C bus, distinguished by address (0x3C vs 0x3D
assumed in config.h). Since these are different physical modules, this is likely but not
guaranteed. **Before final wiring**, connect each display ALONE to any working I2C bus and
run a scanner sketch (search "ESP32 I2C scanner", ~20 lines) to confirm its actual address.
If both eyes and the mouth all report 0x3C with no jumper to change it, you'll need to
re-pair which display shares with which — tell me the actual addresses you find and I'll
adjust config.h in under a minute.

## 1. Arduino IDE setup

1. **Board:** Tools → Board → esp32 → "ESP32S3 Dev Module"
2. **Tools → USB CDC On Boot → Enabled**
3. **Tools → Flash Size → 16MB**
4. **Tools → PSRAM → OPI PSRAM** (critical — this board has octal PSRAM, wrong setting can prevent boot)
5. **Tools → Partition Scheme → Default 4MB with spiffs** (or any 16M-compatible scheme)

### Install required libraries (Library Manager, Tools → Manage Libraries)
- `Adafruit GFX Library`
- `Adafruit SSD1306` (for both eyes)
- `Adafruit SH110X` (for the mouth — 1.3" 128x64 I2C modules are almost always SH1106, not SSD1306)

---

## 2. Wiring — GPIO assignments (already set in config.h)

**⚠️ IMPORTANT:** ESP32-S3-N16R8 has octal PSRAM. GPIO 26-32 and 33-37 are permanently
off-limits — never wire anything there. All pins below are pre-checked against this.

### INMP441 Microphone (I2S input)
| INMP441 Pin | ESP32-S3 Pin |
|---|---|
| VDD | 3.3V |
| GND | GND |
| SCK | GPIO 14 |
| WS | GPIO 15 |
| SD | GPIO 13 |
| L/R | GND (must not float) |

### MAX98357A Amp (I2S output) → YD78-3 speaker
| MAX98357A Pin | ESP32-S3 Pin |
|---|---|
| VIN | 5V |
| GND | GND |
| BCLK | GPIO 17 |
| LRC | GPIO 18 |
| DIN | GPIO 16 |
| GAIN | Floating (9dB default) |
| SD | Floating or tied HIGH |
| Speaker +/− | YD78-3 terminals |

### Left Eye — 0.96" SSD1306 (dedicated I2C bus)
| Pin | ESP32-S3 Pin |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 1 |
| SCL | GPIO 2 |

### Right Eye — 0.96" SSD1306 (SHARED bus with mouth)
| Pin | ESP32-S3 Pin |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 3 |
| SCL | GPIO 4 |

### Mouth — 1.3" SH1106, 4-pin I2C (SHARES bus with right eye — same SDA/SCL pins as above)
| Pin | ESP32-S3 Pin |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 3 (same wire as right eye's SDA) |
| SCL | GPIO 4 (same wire as right eye's SCL) |

This is a real shared bus — both the right eye and mouth physically connect to the SAME
two GPIO pins (3 and 4), just like wiring two I2C sensors onto one bus normally. They're
told apart by I2C address (0x3C vs 0x3D), verified in Step 0 above.

### Push Button (4-pin tactile, wired as a simple momentary switch)
| Button leg | ESP32-S3 Pin |
|---|---|
| One leg (either of a connected pair) | GPIO 5 |
| Opposite leg (either of the other pair) | GND |

Tactile buttons with 4 legs are internally just 2 pairs shorted together when pressed —
pick any one leg from each "side" of the button (usually diagonal legs are the same side;
a continuity check with a multimeter takes 10 seconds if unsure).

---

## 3. Power

- **Primary: USB wall adapter (5V/2A+)** into the ESP32-S3's USB-C port at the booth.
- **Backup: 10,000mAh+ power bank, 2A output.**
- With 3 displays + amp + continuous WiFi (wake-word streaming is now always-on while idle),
  power draw is higher than the original single-shot design — prioritize wall power if at
  all possible for the expo.

---

## 4. Testing order — DON'T SKIP, this is how you find problems fast

1. **I2C scanner** (Step 0 above) — confirm all 3 display addresses BEFORE wiring for real
2. **Displays test** — standalone face snippet from `bot_firmware.ino`, confirms all 3 screens work together on their shared/dedicated buses
3. **Button test** — standalone snippet, confirms clean single-press detection
4. **Speaker test** — standalone tone snippet, confirms MAX98357A + speaker wiring
5. **Mic + VAD test** — standalone snippet, confirms mic wiring AND that VAD_PEAK_THRESHOLD in config.h is well-tuned for your actual room noise (adjust if it's constantly triggering on nothing, or never triggering on speech)
6. **Network + wake protocol test** — start `bridge.py` first, then standalone network snippet, confirms the typed message protocol round-trips correctly
7. **Full integration** — only after all 6 pass individually

---

## 5. Tuning VAD_PEAK_THRESHOLD

This determines how loud something needs to be for the ESP32 to consider it "possible
speech" and either start a wake-check or continue an active session. Too low = constantly
triggers on background noise (wastes network/API calls). Too high = misses quiet speech.

Use the mic standalone test to watch live peak values in a quiet room vs. while talking at
a normal expo-booth distance, then set `VAD_PEAK_THRESHOLD` in config.h to roughly halfway
between your quiet-room baseline and your speaking-voice peak.

---

## 6. If something doesn't work

- **Board won't boot / crashes immediately:** PSRAM setting mismatch (Tools → PSRAM → OPI PSRAM), or a pin wired into GPIO26-37.
- **Only one of right-eye/mouth displays anything:** I2C address collision — re-run the Step 0 scanner, update `EYE_R_I2C_ADDR`/`MOUTH_I2C_ADDR` in config.h to match reality.
- **Mouth display shows shifted/garbled graphics:** Wrong driver — confirm it's really SH1106 (should be, per your module's 4-pin I2C-only spec) and that `MOUTH_IS_SSD1306` is `0` in config.h.
- **No sound from speaker:** Check MAX98357A GAIN/SD pins aren't accidentally grounded.
- **Mic reads all zeros:** Check INMP441 L/R pin is tied to GND, not floating.
- **Wake word never triggers:** Check bridge.py's console — look for `[WAKE] heard: '...'` lines. If it's transcribing garbage, the mic audio quality or VAD threshold likely needs adjustment. If it transcribes correctly but never matches, check `BOT_NAME` matches exactly (case-insensitive, but spelling matters) between config.h and bridge.py.
- **Wake word triggers constantly on nothing:** VAD_PEAK_THRESHOLD is too low, room noise is triggering wake-checks constantly — raise the threshold.
- **Session never times out / never ends automatically:** Check `SESSION_SILENCE_TIMEOUT_MS` in config.h — increase if it's cutting sessions too aggressively during natural pauses, decrease if it lingers too long after you're done talking.
- **WiFi won't connect:** Check SSID/password exactly. Expo WiFi with captive portals won't work — use a phone hotspot instead.
- **Falling back to Serial mode:** Flip `TRANSPORT_MODE` in config.h to `TRANSPORT_SERIAL`, re-upload, run `bridge.py --mode serial --port COMx`.

