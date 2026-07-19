/*
  ============================================================================
  speaker.h — MAX98357A I2S speaker playback module.
  Call speakerInit() once, then speakerPlayPCM() with a buffer of 16-bit PCM
  samples at SAMPLE_RATE_HZ to play audio.

  Standalone test: call speakerInit(), generate a simple sine wave buffer,
  call speakerPlayPCM() — you should hear a tone. This proves the amp/speaker
  wiring works before you ever touch the network code.
  ============================================================================
*/
#pragma once
#include <driver/i2s.h>
#include "config.h"

static bool _speakerOk = false;

inline bool speakerInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE_HZ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = SPK_PIN_BCLK,
    .ws_io_num = SPK_PIN_LRC,
    .data_out_num = SPK_PIN_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err = i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    if (DEBUG_VERBOSE) Serial.printf("[speaker] i2s_driver_install failed: %d\n", err);
    return false;
  }
  err = i2s_set_pin(SPK_I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    if (DEBUG_VERBOSE) Serial.printf("[speaker] i2s_set_pin failed: %d\n", err);
    return false;
  }

  _speakerOk = true;
  if (DEBUG_VERBOSE) Serial.println("[speaker] I2S speaker initialized OK");
  return true;
}

// Blocking playback of a PCM16 mono buffer. For streamed playback (audio
// arriving over network in chunks), call this repeatedly per chunk as data
// arrives — see network.h's handling.
inline void speakerPlayPCM(const int16_t* samples, size_t sampleCount) {
  if (!_speakerOk) return;
  size_t bytesWritten = 0;
  // MAX98357A is mono-summing (L+R)/2 by default with SD floating, so writing
  // the same sample to "left channel only" format still drives it correctly.
  i2s_write(SPK_I2S_PORT, samples, sampleCount * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
}

// Quick self-test tone generator — 440Hz sine for `ms` milliseconds.
// Call this from a bare test sketch to confirm speaker wiring before
// touching any network/API code.
inline void speakerTestTone(int ms) {
  const int freq = 440;
  size_t numSamples = (SAMPLE_RATE_HZ * ms) / 1000;
  int16_t* buf = (int16_t*)malloc(numSamples * sizeof(int16_t));
  if (!buf) return;
  for (size_t i = 0; i < numSamples; i++) {
    float t = (float)i / SAMPLE_RATE_HZ;
    buf[i] = (int16_t)(3000 * sinf(2 * PI * freq * t));
  }
  speakerPlayPCM(buf, numSamples);
  free(buf);
}
