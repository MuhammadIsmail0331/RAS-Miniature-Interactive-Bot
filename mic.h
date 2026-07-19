/*
  ============================================================================
  mic.h — INMP441 I2S microphone capture module.
  Continuously fills a ring buffer with 16-bit PCM samples at SAMPLE_RATE_HZ.
  Call micInit() once, then micTask() must run continuously (put it on its
  own FreeRTOS task — see main.ino) since I2S reads block briefly.

  Standalone test: call micInit(), then in a loop call micReadChunk() and
  print min/max values — you already validated this exact mic works from
  your earlier MAX9814 test, this just swaps to the proper I2S driver.
  ============================================================================
*/
#pragma once
#include <driver/i2s.h>
#include "config.h"

static int16_t* _ringBuffer = nullptr;
static volatile size_t _ringWritePos = 0;
static volatile bool _ringFull = false;
static bool _micOk = false;

inline bool micInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE_HZ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // INMP441 outputs 24-bit in a 32-bit frame
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = MIC_PIN_SCK,
    .ws_io_num = MIC_PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_PIN_SD
  };

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    if (DEBUG_VERBOSE) Serial.printf("[mic] i2s_driver_install failed: %d\n", err);
    return false;
  }
  err = i2s_set_pin(MIC_I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    if (DEBUG_VERBOSE) Serial.printf("[mic] i2s_set_pin failed: %d\n", err);
    return false;
  }

  _ringBuffer = (int16_t*)malloc(AUDIO_BUFFER_SAMPLES * sizeof(int16_t));
  if (_ringBuffer == nullptr) {
    if (DEBUG_VERBOSE) Serial.println("[mic] FAILED to allocate ring buffer — out of memory");
    return false;
  }
  memset(_ringBuffer, 0, AUDIO_BUFFER_SAMPLES * sizeof(int16_t));

  _micOk = true;
  if (DEBUG_VERBOSE) Serial.println("[mic] I2S mic initialized OK");
  return true;
}

// Reads one chunk from I2S and pushes samples into the ring buffer.
// Call this frequently (e.g. every loop iteration or in a dedicated task).
// Returns number of samples read this call (0 if nothing available yet).
inline size_t micTask() {
  if (!_micOk) return 0;

  static int32_t rawBuf[256];
  size_t bytesRead = 0;
  i2s_read(MIC_I2S_PORT, rawBuf, sizeof(rawBuf), &bytesRead, 0); // 0 = non-blocking-ish (short timeout)

  size_t samplesRead = bytesRead / sizeof(int32_t);
  for (size_t i = 0; i < samplesRead; i++) {
    // INMP441 gives 24-bit signed data left-justified in 32 bits — shift down to get a clean 16-bit value
    int16_t sample16 = (int16_t)(rawBuf[i] >> 14);
    _ringBuffer[_ringWritePos] = sample16;
    _ringWritePos = (_ringWritePos + 1) % AUDIO_BUFFER_SAMPLES;
    if (_ringWritePos == 0) _ringFull = true;
  }
  return samplesRead;
}

// Returns a pointer + length representing the last `seconds` of audio,
// linearized (handles ring wraparound) into a freshly allocated buffer.
// Caller must free() the returned pointer.
inline int16_t* micGetLastSeconds(float seconds, size_t* outSampleCount) {
  size_t wantSamples = (size_t)(seconds * SAMPLE_RATE_HZ);
  if (wantSamples > AUDIO_BUFFER_SAMPLES) wantSamples = AUDIO_BUFFER_SAMPLES;

  int16_t* out = (int16_t*)malloc(wantSamples * sizeof(int16_t));
  if (!out) {
    *outSampleCount = 0;
    return nullptr;
  }

  size_t startPos;
  if (_ringFull) {
    startPos = (_ringWritePos + AUDIO_BUFFER_SAMPLES - wantSamples) % AUDIO_BUFFER_SAMPLES;
  } else {
    // Not enough data yet to fill the request — return what we have from position 0
    wantSamples = min(wantSamples, (size_t)_ringWritePos);
    startPos = 0;
  }

  for (size_t i = 0; i < wantSamples; i++) {
    out[i] = _ringBuffer[(startPos + i) % AUDIO_BUFFER_SAMPLES];
  }
  *outSampleCount = wantSamples;
  return out;
}

// Simple peak-level meter, useful for a quick standalone test (like your
// earlier MAX9814 test) to confirm the mic is picking up sound.
inline int16_t micPeakOfLastChunk(int16_t* buf, size_t n) {
  int16_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int16_t v = abs(buf[i]);
    if (v > peak) peak = v;
  }
  return peak;
}

// On-device Voice Activity Detection gate — the ESP32 doing real work rather
// than blindly forwarding everything. Returns true if the most recent
// `seconds` of ring-buffer audio has a peak amplitude above VAD_PEAK_THRESHOLD,
// i.e. "probably contains speech, not just room noise." This is what lets
// the bot listen continuously without spamming the network/API with silence.
inline bool micHasRecentSound(float seconds) {
  size_t n = 0;
  int16_t* buf = micGetLastSeconds(seconds, &n);
  if (!buf || n == 0) { if (buf) free(buf); return false; }
  int16_t peak = micPeakOfLastChunk(buf, n);
  free(buf);
  return peak > VAD_PEAK_THRESHOLD;
}

