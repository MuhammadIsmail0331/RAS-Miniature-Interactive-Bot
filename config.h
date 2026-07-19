/*
  ============================================================================
  config.h — ALL pins, settings, and mode switches live here.
  If something's wired differently than assumed, or you need to swap a pin
  because of a conflict, change it HERE ONLY. Every other file references
  these names, never a hardcoded pin number.

  BOARD: ESP32-S3-N16R8 (16MB flash, 8MB OCTAL PSRAM)
  ⚠️ GPIO 26-32 (flash) and 33-37 (octal PSRAM) are PERMANENTLY OFF-LIMITS.
  Every pin below has been checked against this. Do not add a pin in that
  range even temporarily for debugging — it can crash or corrupt the board.
  Safe pins used elsewhere in this project: 1-18, 21, 38-42, 47.
  ============================================================================
*/
#pragma once

// ---------------------------------------------------------------------------
// TRANSPORT MODE — flip this ONE line to switch between WiFi and USB Serial
// if expo WiFi turns out to be unreliable. No other code changes needed.
// ---------------------------------------------------------------------------
#define TRANSPORT_WIFI     1
#define TRANSPORT_SERIAL   2
#define TRANSPORT_MODE     TRANSPORT_WIFI   // <-- change to TRANSPORT_SERIAL if WiFi fails

// ---------------------------------------------------------------------------
// WIFI SETTINGS (only used if TRANSPORT_MODE == TRANSPORT_WIFI)
// ---------------------------------------------------------------------------
#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PASSWORD      "YOUR_WIFI_PASSWORD"
#define PC_SERVER_IP       "192.168.1.100"   // <-- set to your laptop's LAN IP (run `ipconfig` on PC)
#define PC_SERVER_PORT     8765
#define WIFI_CONNECT_TIMEOUT_MS   15000

// ---------------------------------------------------------------------------
// I2S MICROPHONE (INMP441) PINS
// ---------------------------------------------------------------------------
#define MIC_I2S_PORT       I2S_NUM_0
#define MIC_PIN_SCK        14   // Serial clock (BCLK)
#define MIC_PIN_WS         15   // Word select (LRCLK)
#define MIC_PIN_SD         13   // Serial data out from mic
// INMP441 L/R pin: tie to GND for left channel — must NOT be left floating.

// ---------------------------------------------------------------------------
// I2S SPEAKER AMP (MAX98357A) PINS
// Separate I2S peripheral from the mic (S3 has two), so record/playback
// never contend for the same bus — avoids a class of timing glitches.
// ---------------------------------------------------------------------------
#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_PIN_BCLK       17
#define SPK_PIN_LRC        18
#define SPK_PIN_DIN        16
// MAX98357A GAIN pin: leave floating = 9dB (good default)
// MAX98357A SD pin: leave floating or tie HIGH = enabled

// ---------------------------------------------------------------------------
// DISPLAYS — TWO hardware I2C buses (the S3 only has two real I2C
// peripherals — Wire and Wire1 — there is no automatic third hardware bus).
//
// SIMPLIFIED DESIGN: both eyes are wired IN PARALLEL on the SAME bus, at the
// SAME address (0x3C). This works because you want both eyes to always show
// the identical image — since they're indistinguishable to the I2C bus,
// ONE draw command updates both physical screens simultaneously. Only one
// Adafruit_SSD1306 object is used in software; both eyes just happen to be
// wired to hear it. No solder-jumper modification needed on either eye board.
//
// The mouth gets its OWN dedicated bus (different chip/address anyway —
// SH1106 @ 0x3C, confirmed by your I2C scan — no collision possible since
// it's alone on its bus).
// ---------------------------------------------------------------------------
// Eyes (0.96" SSD1306 x2, wired in parallel) — bus 0 (Wire)
#define EYES_PIN_SDA       3
#define EYES_PIN_SCL       4
#define EYES_I2C_ADDR      0x3C
#define EYE_WIDTH          128
#define EYE_HEIGHT         64

// Mouth (1.3" SH1106, 4-pin I2C-only module) — its OWN dedicated bus (Wire1).
// Confirmed via I2C scanner: this board is at 0x3C (its factory default).
// Since it's alone on this bus, no address collision is possible regardless
// of what address it uses. Uses Adafruit_SH110X library — 1.3" 128x64 I2C
// boards are almost always SH1106, not SSD1306, despite looking identical
// and having the same resolution; wrong driver = shifted/garbled display.
#define MOUTH_PIN_SDA      1
#define MOUTH_PIN_SCL      2
#define MOUTH_I2C_ADDR     0x3C   // confirmed by your I2C scan
#define MOUTH_WIDTH        128
#define MOUTH_HEIGHT       64
#define MOUTH_IS_SSD1306   0   // set to 1 if your 1.3" module's listing/silkscreen confirms SSD1306, not SH1106

// ---------------------------------------------------------------------------
// PUSH BUTTON (4-pin tactile, functions as simple momentary switch)
// ---------------------------------------------------------------------------
#define BUTTON_PIN         5
#define BUTTON_ACTIVE_LOW  true   // true if button connects pin to GND when pressed (use INPUT_PULLUP)
#define BUTTON_DEBOUNCE_MS 40

// ---------------------------------------------------------------------------
// AUDIO CAPTURE SETTINGS
// ---------------------------------------------------------------------------
#define SAMPLE_RATE_HZ         16000     // 16kHz mono is plenty for speech, keeps buffers small
#define SAMPLE_BITS             16
#define RECORD_SECONDS            8      // max length of a single ring-buffer window
#define AUDIO_BUFFER_SAMPLES   (SAMPLE_RATE_HZ * RECORD_SECONDS)
#define AUDIO_BUFFER_BYTES     (AUDIO_BUFFER_SAMPLES * (SAMPLE_BITS / 8))

// ---------------------------------------------------------------------------
// WAKE WORD / CONVERSATION SESSION SETTINGS
// ---------------------------------------------------------------------------
// Bot's name — spoken by the PC bridge's wake-word check. Change freely.
#define BOT_NAME                    "Aria"

// While IDLE (not in an active conversation), the ESP32 sends short rolling
// chunks to the PC for wake-word checking. Short = fast to check, cheap,
// low-latency wake response. Not the same as a full conversational utterance.
#define WAKE_CHUNK_SECONDS          2.0

// Simple on-device voice activity gate BEFORE sending anything to the PC —
// this is the ESP32 doing real work, not just relaying raw audio blindly.
// Chunks below this peak amplitude are silence/noise and are never sent,
// which is what keeps continuous listening from spamming the network/API
// nonstop while the room is just quiet ambient noise.
#define VAD_PEAK_THRESHOLD          400

// Once a conversation session is ACTIVE (triggered by button or wake word),
// how long to wait with no detected speech before auto-ending the session
// and going back to idle/wake-word-only listening.
#define SESSION_SILENCE_TIMEOUT_MS  6000

// Max length of a single spoken turn within an active session before we
// cut and send anyway (prevents a stuck-open mic from buffering forever).
#define SESSION_MAX_UTTERANCE_SEC   12

// ---------------------------------------------------------------------------
// PLAYBACK BUFFER
// ---------------------------------------------------------------------------
#define PLAYBACK_CHUNK_BYTES    4096

// ---------------------------------------------------------------------------
// DEBUG
// ---------------------------------------------------------------------------
#define DEBUG_SERIAL_BAUD     115200
#define DEBUG_VERBOSE          true   // set false to quiet down Serial prints once stable
