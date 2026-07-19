/*
  ============================================================================
  button.h — Debounced push button. Detects a single clean "press" event.
  Call buttonInit() once, then buttonWasPressed() each loop — it returns
  true exactly once per physical press (debounced), not continuously while held.
  ============================================================================
*/
#pragma once
#include "config.h"

static bool _buttonLastRawState = false;
static bool _buttonStableState = false;
static unsigned long _buttonLastChangeMillis = 0;
static bool _buttonConsumed = true; // starts consumed so no phantom press at boot

inline void buttonInit() {
  pinMode(BUTTON_PIN, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  if (DEBUG_VERBOSE) Serial.println("[button] initialized");
}

// Call every loop() iteration. Returns true exactly once per debounced press.
inline bool buttonWasPressed() {
  bool raw = digitalRead(BUTTON_PIN);
  bool pressedRaw = BUTTON_ACTIVE_LOW ? !raw : raw;

  if (pressedRaw != _buttonLastRawState) {
    _buttonLastChangeMillis = millis();
    _buttonLastRawState = pressedRaw;
  }

  if ((millis() - _buttonLastChangeMillis) > BUTTON_DEBOUNCE_MS) {
    if (pressedRaw != _buttonStableState) {
      _buttonStableState = pressedRaw;
      if (_buttonStableState == true) {
        _buttonConsumed = false; // fresh press, not yet reported
      }
    }
  }

  if (_buttonStableState && !_buttonConsumed) {
    _buttonConsumed = true;
    return true;
  }
  return false;
}
