/*
  ============================================================================
  face.h — Face module: eyes (SSD1306, wired in parallel as ONE logical
  display) + mouth (SH1106, own dedicated bus).

  EYES DESIGN: both physical eye screens are wired to the SAME I2C bus at
  the SAME address (0x3C) — see config.h for the reasoning. This means only
  ONE Adafruit_SSD1306 object exists in software; every draw/display() call
  updates both physical screens simultaneously, since electrically they
  can't be told apart. This is intentional and correct for this project
  (both eyes always show the same expression).

  MOUTH: separate chip (SH1106) on its own dedicated bus (Wire1) — confirmed
  via I2C scanner at 0x3C, cannot collide with anything since it's alone on
  that bus.

  Call faceInit() once, then faceSetState() when the bot's status changes,
  and faceUpdate() every loop() iteration (self-throttles its own animation
  timing internally, safe to call at full loop speed).

  This file has NO dependency on audio/network code — fully testable alone.
  ============================================================================
*/
#pragma once
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include "config.h"

enum FaceState {
  FACE_IDLE,        // slow blink, mouth closed — waiting for wake word or button
  FACE_LISTENING,   // eyes wide, mouth closed — active session, capturing speech
  FACE_THINKING,    // eyes narrowed, mouth shows "..." — waiting on PC/API response
  FACE_TALKING,     // eyes normal, mouth animates like speaking — playing response
  FACE_ERROR        // eyes show concern, mouth shows error indicator
};

// Both SSD1306_WHITE and SH110X_WHITE equal 1 in their respective libraries,
// but the macro NAME differs — this picks the right one at compile time so
// mouth drawing code works regardless of which driver MOUTH_IS_SSD1306 selects.
#if MOUTH_IS_SSD1306
  #define MOUTH_WHITE SSD1306_WHITE
#else
  #define MOUTH_WHITE SH110X_WHITE
#endif

// Two hardware I2C buses: bus 0 drives BOTH eyes in parallel (one object,
// two physical screens listening), bus 1 drives the mouth alone.
static TwoWire &_wireEyes = Wire;
static TwoWire &_wireMouth = Wire1;

static Adafruit_SSD1306 _eyes(EYE_WIDTH, EYE_HEIGHT, &_wireEyes, -1);
#if MOUTH_IS_SSD1306
static Adafruit_SSD1306 _mouth(MOUTH_WIDTH, MOUTH_HEIGHT, &_wireMouth, -1);
#else
static Adafruit_SH1106G _mouth(MOUTH_WIDTH, MOUTH_HEIGHT, &_wireMouth, -1);
#endif

static FaceState _currentState = FACE_IDLE;
static unsigned long _lastAnimMillis = 0;
static uint8_t _animFrame = 0;
static bool _eyesOk = false, _mouthOk = false;

inline bool faceInit() {
  bool allOk = true;

  _wireEyes.begin(EYES_PIN_SDA, EYES_PIN_SCL);
  _eyesOk = _eyes.begin(SSD1306_SWITCHCAPVCC, EYES_I2C_ADDR);
  if (!_eyesOk) { Serial.println("[face] Eyes SSD1306 init FAILED"); allOk = false; }
  else { _eyes.clearDisplay(); _eyes.display(); }

  _wireMouth.begin(MOUTH_PIN_SDA, MOUTH_PIN_SCL);
#if MOUTH_IS_SSD1306
  _mouthOk = _mouth.begin(SSD1306_SWITCHCAPVCC, MOUTH_I2C_ADDR);
#else
  _mouthOk = _mouth.begin(MOUTH_I2C_ADDR, true);
#endif
  if (!_mouthOk) { Serial.println("[face] Mouth init FAILED"); allOk = false; }
  else { _mouth.clearDisplay(); _mouth.display(); }

  if (DEBUG_VERBOSE) {
    Serial.printf("[face] eyes:%s mouth:%s\n",
      _eyesOk ? "OK" : "FAIL", _mouthOk ? "OK" : "FAIL");
  }
  return allOk;
}

inline void faceSetState(FaceState s) {
  if (s != _currentState) {
    _currentState = s;
    _animFrame = 0;
  }
}

// --- Eye drawing (drives both physical screens via the one shared object) ---
inline void _drawEyes(int openness) {
  // openness: 0 (closed/blink) to 100 (wide open)
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  int w = 46;
  int h = map(openness, 0, 100, 3, 46);
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx - w/2, cy - h/2, w, h, 8, SSD1306_WHITE);
  _eyes.display();
}

inline void _drawEyesListening() {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx - 25, cy - 23, 50, 46, 10, SSD1306_WHITE); // wide open
  _eyes.display();
}

inline void _drawEyesThinking(uint8_t frame) {
  // narrowed, with a subtle "looking around" horizontal shift
  int cx = EYE_WIDTH / 2 + ((frame % 4 == 1) ? 6 : (frame % 4 == 3 ? -6 : 0));
  int cy = EYE_HEIGHT / 2;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx - 23, cy - 12, 46, 24, 8, SSD1306_WHITE);
  _eyes.display();
}

// --- Mouth drawing ---
inline void _drawMouthClosed() {
  int cx = MOUTH_WIDTH / 2;
  int cy = MOUTH_HEIGHT / 2;
  _mouth.clearDisplay();
  _mouth.fillRoundRect(cx - 30, cy - 3, 60, 6, 3, MOUTH_WHITE);
  _mouth.display();
}

inline void _drawMouthThinking(uint8_t frame) {
  _mouth.clearDisplay();
  int cy = MOUTH_HEIGHT / 2;
  int spacing = 20;
  int startX = MOUTH_WIDTH / 2 - spacing;
  for (int i = 0; i < 3; i++) {
    int dotSize = (frame % 3 == i) ? 7 : 4;
    _mouth.fillCircle(startX + i * spacing, cy, dotSize, MOUTH_WHITE);
  }
  _mouth.display();
}

inline void _drawMouthTalking(uint8_t frame) {
  _mouth.clearDisplay();
  int cx = MOUTH_WIDTH / 2;
  int cy = MOUTH_HEIGHT / 2;
  // cycles through a few mouth-open heights to look like talking
  int shapes[4] = {6, 20, 12, 26};
  int h = shapes[frame % 4];
  _mouth.fillRoundRect(cx - 26, cy - h/2, 52, h, 6, MOUTH_WHITE);
  _mouth.display();
}

inline void _drawMouthError() {
  _mouth.clearDisplay();
  _mouth.setTextSize(1);
  _mouth.setCursor(18, MOUTH_HEIGHT/2 - 4);
  _mouth.print("connection error");
  _mouth.display();
}

// Call every loop() iteration — self-throttles animation timing internally.
inline void faceUpdate() {
  unsigned long now = millis();
  unsigned long frameInterval;

  switch (_currentState) {
    case FACE_IDLE:       frameInterval = 2500; break; // slow blink
    case FACE_LISTENING:  frameInterval = 500;  break; // mostly static, small idle motion
    case FACE_THINKING:   frameInterval = 300;  break;
    case FACE_TALKING:    frameInterval = 140;  break; // fast enough to look like real speech
    case FACE_ERROR:      frameInterval = 400;  break;
    default:              frameInterval = 300;  break;
  }

  if (now - _lastAnimMillis < frameInterval) return;
  _lastAnimMillis = now;
  _animFrame++;

  switch (_currentState) {
    case FACE_IDLE: {
      bool blink = (_animFrame % 6 == 0);
      if (_eyesOk) _drawEyes(blink ? 5 : 100);
      if (_mouthOk) _drawMouthClosed();
      break;
    }
    case FACE_LISTENING:
      if (_eyesOk) _drawEyesListening();
      if (_mouthOk) _drawMouthClosed();
      break;
    case FACE_THINKING:
      if (_eyesOk) _drawEyesThinking(_animFrame);
      if (_mouthOk) _drawMouthThinking(_animFrame);
      break;
    case FACE_TALKING:
      if (_eyesOk) _drawEyes(100);
      if (_mouthOk) _drawMouthTalking(_animFrame);
      break;
    case FACE_ERROR:
      if (_eyesOk) _drawEyes(30);
      if (_mouthOk && (_animFrame % 2 == 0)) _drawMouthError();
      break;
  }
}
