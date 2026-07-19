/*
  ============================================================================
  face.h — Face module: eyes (SSD1306, wired in parallel as ONE logical
  display) + mouth (SH1106, own dedicated bus).

  ANIMATION DESIGN: each state has real shape variety and continuous motion,
  not a single shape resized. Prototyped and visually verified in Python/PIL
  before translation here — see render_preview.py frame strips. Key ideas:
    - IDLE: natural blink cycle + occasional glance + occasional sleepy
      half-blink, so it never sits fully static
    - LISTENING: genuinely wider than idle, with a slow attentive side-to-side
      sweep (sine-driven, not a twitch)
    - THINKING: squinted eyes doing a real circular "searching" orbit motion;
      mouth keeps the ellipsis (per feedback this one already read well) but
      with a proper traveling-wave bounce through the three dots
    - TALKING: eyes pulse gently in sync with an energetic multi-shape mouth
      cycle (O/E/small/wide — a real talk-shape vocabulary, not one bar
      growing and shrinking)
    - ERROR: a visually distinct worried eye tilt + a wobbling mouth line
      (reads instantly, faster than text at a glance)

  EYES DESIGN: both physical eye screens are wired to the SAME I2C bus at
  the SAME address (0x3C) — see config.h for the reasoning. Only ONE
  Adafruit_SSD1306 object exists in software; every draw/display() call
  updates both physical screens simultaneously.

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
#include <math.h>
#include "config.h"

enum FaceState {
  FACE_IDLE,        // natural blink/glance cycle, mouth closed with occasional smile pulse
  FACE_LISTENING,   // wide alert eyes with attentive sweep, neutral mouth
  FACE_THINKING,    // squinted eyes orbiting, mouth ellipsis with traveling wave
  FACE_TALKING,     // pulsing eyes, energetic multi-shape talk cycle
  FACE_ERROR        // worried tilted eyes, wobbling mouth line
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
static uint16_t _animFrame = 0;   // wider than before — idle's cycle needs up to 40 frames
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

// ---------------------------------------------------------------------------
// EYES
// ---------------------------------------------------------------------------

inline void _drawEyesIdle(uint16_t frame) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  uint16_t cycle = frame % 40;

  int glanceX = 0;
  if (cycle >= 15 && cycle < 19) glanceX = -10;
  else if (cycle >= 19 && cycle < 23) glanceX = 10;

  int h;
  if (cycle < 2) h = 4;                          // quick full blink
  else if (cycle >= 30 && cycle < 32) h = 20;     // sleepy half-blink
  else h = 40;

  int w = 52;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx + glanceX - w/2, cy - h/2, w, h, 12, SSD1306_WHITE);
  _eyes.display();
}

inline void _drawEyesListening(uint16_t frame) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  float sweep = sinf(frame * 0.15f) * 8.0f;
  int w = 58, h = 50;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx + (int)sweep - w/2, cy - h/2, w, h, 14, SSD1306_WHITE);
  _eyes.display();
}

inline void _drawEyesThinking(uint16_t frame) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  float angle = frame * 0.35f;
  float orbitR = 10.0f;
  int ox = (int)(cosf(angle) * orbitR);
  int oy = (int)(sinf(angle) * orbitR * 0.4f);
  int w = 50, h = 22;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx + ox - w/2, cy + oy - h/2, w, h, 10, SSD1306_WHITE);
  _eyes.display();
}

inline void _drawEyesTalking(uint16_t frame) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  float pulse = sinf(frame * 0.5f) * 3.0f;
  int w = 54, h = 42 + (int)pulse;
  _eyes.clearDisplay();
  _eyes.fillRoundRect(cx - w/2, cy - h/2, w, h, 12, SSD1306_WHITE);
  _eyes.display();
}

inline void _drawEyesError(uint16_t frame) {
  int cx = EYE_WIDTH / 2;
  int cy = EYE_HEIGHT / 2;
  float wobble = sinf(frame * 0.3f) * 2.0f;
  int w = 48, h = 18;
  // Tilted polygon suggesting a worried brow — visually distinct from every
  // other state's straight rounded-rect, reads as "wrong" at a glance.
  int16_t xs[4] = { (int16_t)(cx - w/2), (int16_t)(cx + w/2), (int16_t)(cx + w/2), (int16_t)(cx - w/2) };
  int16_t ys[4] = {
    (int16_t)(cy - h/2 + wobble),
    (int16_t)(cy - h/2 - 6 + wobble),
    (int16_t)(cy + h/2 - 6 + wobble),
    (int16_t)(cy + h/2 + wobble)
  };
  _eyes.clearDisplay();
  _eyes.fillTriangle(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], SSD1306_WHITE);
  _eyes.fillTriangle(xs[0], ys[0], xs[2], ys[2], xs[3], ys[3], SSD1306_WHITE);
  _eyes.display();
}

// ---------------------------------------------------------------------------
// MOUTH
// ---------------------------------------------------------------------------

inline void _drawMouthIdle(uint16_t frame) {
  int cx = MOUTH_WIDTH / 2;
  int cy = MOUTH_HEIGHT / 2;
  uint16_t cycle = frame % 60;
  _mouth.clearDisplay();

  if (cycle >= 40 && cycle < 55) {
    // occasional subtle smile-curve pulse, so idle mouth isn't dead static.
    // Simple approach using standard Adafruit_GFX primitives only (no drawArc
    // — that function only exists in third-party forks, not the official
    // library, and would fail to compile).
    float t = (cycle - 40) / 15.0f;
    float curveAmt = sinf(t * PI) * 4.0f;
    int w = 44;
    _mouth.fillRoundRect(cx - w/2, cy - 3 - (int)(curveAmt/2), w, 6, 3, MOUTH_WHITE);
  } else {
    _mouth.fillRoundRect(cx - 22, cy - 3, 44, 5, 2, MOUTH_WHITE);
  }
  _mouth.display();
}

inline void _drawMouthListening() {
  int cx = MOUTH_WIDTH / 2;
  int cy = MOUTH_HEIGHT / 2;
  _mouth.clearDisplay();
  _mouth.fillRoundRect(cx - 20, cy - 3, 40, 5, 2, MOUTH_WHITE);
  _mouth.display();
}

inline void _drawMouthThinking(uint16_t frame) {
  _mouth.clearDisplay();
  int cy = MOUTH_HEIGHT / 2;
  int spacing = 20;
  int startX = MOUTH_WIDTH / 2 - spacing;
  for (int i = 0; i < 3; i++) {
    float phase = (frame * 0.4f) - i * 1.2f;
    float bounce = fmaxf(0.0f, sinf(phase)) * 4.0f;
    int size = 4 + (int)bounce;
    _mouth.fillCircle(startX + i * spacing, cy, size, MOUTH_WHITE);
  }
  _mouth.display();
}

inline void _drawMouthTalking(uint16_t frame) {
  int cx = MOUTH_WIDTH / 2;
  int cy = MOUTH_HEIGHT / 2;
  // Real talk-shape vocabulary — varied widths/heights, not one bar resizing.
  static const int8_t shapes[6][3] = {
    {30, 8, 6},    // small flat
    {26, 26, 13},  // round "O"
    {44, 10, 5},   // wide "E"
    {34, 18, 9},   // medium open
    {20, 6, 3},    // near closed
    {40, 22, 11},  // wide open
  };
  int idx = frame % 6;
  int w = shapes[idx][0], h = shapes[idx][1], r = shapes[idx][2];
  _mouth.clearDisplay();
  _mouth.fillRoundRect(cx - w/2, cy - h/2, w, h, r, MOUTH_WHITE);
  _mouth.display();
}

inline void _drawMouthError(uint16_t frame) {
  _mouth.clearDisplay();
  int cy = MOUTH_HEIGHT / 2;
  int startX = MOUTH_WIDTH / 2 - 30;
  int endX = MOUTH_WIDTH / 2 + 30;
  int prevX = startX, prevY = cy;
  for (int x = startX; x <= endX; x += 2) {
    int y = cy + (int)(sinf((x + frame * 4) * 0.3f) * 5.0f);
    _mouth.drawLine(prevX, prevY, x, y, MOUTH_WHITE);
    _mouth.drawLine(prevX, prevY + 1, x, y + 1, MOUTH_WHITE); // thicken to width ~2px
    prevX = x; prevY = y;
  }
  _mouth.display();
}

// Call every loop() iteration — self-throttles animation timing internally.
inline void faceUpdate() {
  unsigned long now = millis();
  unsigned long frameInterval;

  switch (_currentState) {
    case FACE_IDLE:       frameInterval = 140;  break; // needs frequent ticks for smooth 40-frame blink/glance cycle
    case FACE_LISTENING:  frameInterval = 100;  break; // smooth sweep motion
    case FACE_THINKING:   frameInterval = 100;  break; // smooth orbit motion
    case FACE_TALKING:    frameInterval = 130; break; // fast enough to look like real speech, not too flickery
    case FACE_ERROR:      frameInterval = 120;  break; // smooth wobble
    default:              frameInterval = 130; break;
  }

  if (now - _lastAnimMillis < frameInterval) return;
  _lastAnimMillis = now;
  _animFrame++;

  switch (_currentState) {
    case FACE_IDLE:
      if (_eyesOk) _drawEyesIdle(_animFrame);
      if (_mouthOk) _drawMouthIdle(_animFrame);
      break;
    case FACE_LISTENING:
      if (_eyesOk) _drawEyesListening(_animFrame);
      if (_mouthOk) _drawMouthListening();
      break;
    case FACE_THINKING:
      if (_eyesOk) _drawEyesThinking(_animFrame);
      if (_mouthOk) _drawMouthThinking(_animFrame);
      break;
    case FACE_TALKING:
      if (_eyesOk) _drawEyesTalking(_animFrame);
      if (_mouthOk) _drawMouthTalking(_animFrame);
      break;
    case FACE_ERROR:
      if (_eyesOk) _drawEyesError(_animFrame);
      if (_mouthOk) _drawMouthError(_animFrame);
      break;
  }
}
