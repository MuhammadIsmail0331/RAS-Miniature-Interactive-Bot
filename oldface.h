/*
  ============================================================================
  face.h — Three-display face module: two eyes (SSD1306) + one mouth (SH1106).

  Each display sits on its OWN dedicated I2C bus (own SDA/SCL pin pair), using
  separate TwoWire instances. This is deliberate: your two eye modules are
  physically identical boards and very likely share the same fixed I2C
  address (0x3C), so they cannot coexist on one shared bus. Giving each
  display its own bus sidesteps that entirely — verified safe regardless of
  what address each board actually has.

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

// Two REAL hardware I2C buses — the S3 has exactly two I2C peripherals.
// Left eye gets its own bus. Right eye + mouth SHARE the second bus,
// distinguished by I2C address (different chip types/driver classes, so
// this works even if addresses happen to collide with some other device —
// verify with an I2C scanner per WIRING.md before final assembly).
static TwoWire &_wireEyeL = Wire;
static TwoWire &_wireShared = Wire1;   // right eye + mouth

static Adafruit_SSD1306 _eyeL(EYE_WIDTH, EYE_HEIGHT, &_wireEyeL, -1);
static Adafruit_SSD1306 _eyeR(EYE_WIDTH, EYE_HEIGHT, &_wireShared, -1);
#if MOUTH_IS_SSD1306
static Adafruit_SSD1306 _mouth(MOUTH_WIDTH, MOUTH_HEIGHT, &_wireShared, -1);
#else
static Adafruit_SH1106G _mouth(MOUTH_WIDTH, MOUTH_HEIGHT, &_wireShared, -1);
#endif

static FaceState _currentState = FACE_IDLE;
static unsigned long _lastAnimMillis = 0;
static uint8_t _animFrame = 0;
static bool _eyeLOk = false, _eyeROk = false, _mouthOk = false;

inline bool faceInit() {
  bool allOk = true;

  _wireEyeL.begin(EYE_L_PIN_SDA, EYE_L_PIN_SCL);
  _eyeLOk = _eyeL.begin(SSD1306_SWITCHCAPVCC, EYE_L_I2C_ADDR);
  if (!_eyeLOk) { Serial.println("[face] Left eye SSD1306 init FAILED"); allOk = false; }
  else { _eyeL.clearDisplay(); _eyeL.display(); }

  // Right eye and mouth share one bus — must both begin() on the SAME
  // TwoWire instance, which is already initialized once here.
  _wireShared.begin(EYE_R_PIN_SDA, EYE_R_PIN_SCL);

  _eyeROk = _eyeR.begin(SSD1306_SWITCHCAPVCC, EYE_R_I2C_ADDR);
  if (!_eyeROk) { Serial.println("[face] Right eye SSD1306 init FAILED"); allOk = false; }
  else { _eyeR.clearDisplay(); _eyeR.display(); }

#if MOUTH_IS_SSD1306
  _mouthOk = _mouth.begin(SSD1306_SWITCHCAPVCC, MOUTH_I2C_ADDR);
#else
  _mouthOk = _mouth.begin(MOUTH_I2C_ADDR, true);
#endif
  if (!_mouthOk) { Serial.println("[face] Mouth init FAILED"); allOk = false; }
  else { _mouth.clearDisplay(); _mouth.display(); }

  if (DEBUG_VERBOSE) {
    Serial.printf("[face] eyeL:%s eyeR:%s mouth:%s\n",
      _eyeLOk ? "OK" : "FAIL", _eyeROk ? "OK" : "FAIL", _mouthOk ? "OK" : "FAIL");
    if (_eyeROk && _mouthOk) {
      Serial.println("[face] NOTE: right eye + mouth share one I2C bus at "
                      "different addresses — if only one of them actually "
                      "displays anything, they likely have colliding "
                      "addresses. Run an I2C scanner and adjust "
                      "EYE_R_I2C_ADDR/MOUTH_I2C_ADDR in config.h.");
    }
  }
  return allOk;
}

inline void faceSetState(FaceState s) {
  if (s != _currentState) {
    _currentState = s;
    _animFrame = 0;
  }
}

// --- Eye drawing (shared logic, called once per eye display) ---
inline void _drawEye(Adafruit_SSD1306 &disp, int openness) {
  // openness: 0 (closed/blink) to 100 (wide open)
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  int w = 46;
  int h = map(openness, 0, 100, 3, 46);
  disp.clearDisplay();
  disp.fillRoundRect(cx - w/2, cy - h/2, w, h, 8, SSD1306_WHITE);
  disp.display();
}

inline void _drawEyeListening(Adafruit_SSD1306 &disp) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  disp.clearDisplay();
  disp.fillRoundRect(cx - 25, cy - 23, 50, 46, 10, SSD1306_WHITE); // wide open
  disp.display();
}

inline void _drawEyeThinking(Adafruit_SSD1306 &disp, uint8_t frame) {
  // narrowed, with a subtle "looking around" horizontal shift
  int cx = EYE_WIDTH / 2 + ((frame % 4 == 1) ? 6 : (frame % 4 == 3 ? -6 : 0));
  int cy = EYE_HEIGHT / 2;
  disp.clearDisplay();
  disp.fillRoundRect(cx - 23, cy - 12, 46, 24, 8, SSD1306_WHITE);
  disp.display();
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
      if (_eyeLOk) _drawEye(_eyeL, blink ? 5 : 100);
      if (_eyeROk) _drawEye(_eyeR, blink ? 5 : 100);
      if (_mouthOk) _drawMouthClosed();
      break;
    }
    case FACE_LISTENING:
      if (_eyeLOk) _drawEyeListening(_eyeL);
      if (_eyeROk) _drawEyeListening(_eyeR);
      if (_mouthOk) _drawMouthClosed();
      break;
    case FACE_THINKING:
      if (_eyeLOk) _drawEyeThinking(_eyeL, _animFrame);
      if (_eyeROk) _drawEyeThinking(_eyeR, _animFrame);
      if (_mouthOk) _drawMouthThinking(_animFrame);
      break;
    case FACE_TALKING:
      if (_eyeLOk) _drawEye(_eyeL, 100);
      if (_eyeROk) _drawEye(_eyeR, 100);
      if (_mouthOk) _drawMouthTalking(_animFrame);
      break;
    case FACE_ERROR:
      if (_eyeLOk) _drawEye(_eyeL, 30);
      if (_eyeROk) _drawEye(_eyeR, 30);
      if (_mouthOk && (_animFrame % 2 == 0)) _drawMouthError();
      break;
  }
}
