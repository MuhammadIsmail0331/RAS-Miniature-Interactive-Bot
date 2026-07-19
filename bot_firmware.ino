/*
  ============================================================================
  bot_firmware.ino — Interactive Bot main sketch (ESP32-S3-N16R8)

  NEW BEHAVIOR (session-based conversation, replaces single-shot capture):

    IDLE (always listening in background):
      - ESP32 runs on-device VAD (micHasRecentSound) constantly, for free,
        with zero network traffic while the room is quiet.
      - The instant VAD detects sound, a short WAKE_CHUNK is sent to the PC
        to ask "was the wake word said?" — this is the ONLY time network
        traffic happens while idle, so idle silence costs nothing.
      - Button press ALSO enters an active session directly, skipping the
        wake-word check entirely (manual override, always works even if
        wake-word detection has a bad day).

    ACTIVE SESSION (started by wake word OR button):
      - Face shows LISTENING. Mic keeps capturing.
      - On each detected utterance (VAD-gated), sends a CONVERSATION message
        with that utterance's audio, gets back a spoken reply, plays it.
      - Session continues automatically — no need to press the button again
        between turns.
      - Session ends when: button is pressed again (manual stop), OR no
        speech detected for SESSION_SILENCE_TIMEOUT_MS (auto stop).

  MODULE MAP (each file independently testable — see bottom of this file
  for standalone test snippets):
    config.h    - all pins/settings, single source of truth
    mic.h       - I2S mic capture (INMP441) + on-device VAD gate
    speaker.h   - I2S playback (MAX98357A)
    face.h      - 3-display face: 2 eyes (SSD1306) + mouth (SH1106)
    button.h    - debounced push button (manual session toggle)
    network.h   - WiFi TCP or USB Serial transport, typed message protocol
  ============================================================================
*/
#include "config.h"
#include "mic.h"
#include "speaker.h"
#include "face.h"
#include "button.h"
#include "network.h"

enum BotState {
  STATE_IDLE_LISTENING,     // background VAD only, no active session
  STATE_WAKE_CHECKING,      // sound detected, asking PC "was the name said?"
  STATE_SESSION_LISTENING,  // active session, waiting for user to speak
  STATE_SESSION_SENDING,    // utterance captured, sending to PC
  STATE_SESSION_WAITING,    // waiting for PC's spoken reply
  STATE_SESSION_PLAYING,    // playing the reply
  STATE_ERROR
};

static BotState _botState = STATE_IDLE_LISTENING;
static unsigned long _errorStateEnteredAt = 0;
static unsigned long _lastSpeechDetectedAt = 0;
static unsigned long _sessionUtteranceStartedAt = 0;
static bool _capturingUtterance = false;

void setup() {
  Serial.begin(DEBUG_SERIAL_BAUD);
  delay(300);
  Serial.println("\n=== Interactive Bot booting ===");
  Serial.printf("Wake word: \"%s\"\n", BOT_NAME);

  bool micOk = micInit();
  bool spkOk = speakerInit();
  bool faceOk = faceInit();
  buttonInit();
  bool netOk = transportInit();

  Serial.println("--- Boot summary ---");
  Serial.printf("  Mic:     %s\n", micOk ? "OK" : "FAILED");
  Serial.printf("  Speaker: %s\n", spkOk ? "OK" : "FAILED");
  Serial.printf("  Faces:   %s\n", faceOk ? "OK" : "PARTIAL/FAILED (check per-display log above)");
  Serial.printf("  Network: %s\n", netOk ? "OK" : "FAILED");
  Serial.println("--------------------");

  if (!micOk || !spkOk || !netOk) {
    Serial.println("[FATAL] A required module failed to init. See WIRING.md.");
    _botState = STATE_ERROR;
    _errorStateEnteredAt = millis();
  } else {
    Serial.println("=== Bot ready. Idle-listening for wake word or button. ===");
    faceSetState(FACE_IDLE);
  }
}

// Ends the active session and returns to idle listening — single place this
// happens so the logic (and logging) is consistent everywhere it's called from.
void endSession(const char* reason) {
  Serial.printf("[main] Session ended (%s). Back to idle.\n", reason);
  _botState = STATE_IDLE_LISTENING;
  _capturingUtterance = false;
  faceSetState(FACE_IDLE);
}

void loop() {
  faceUpdate();
  micTask(); // always keep ring buffer filling, in every state

  // Button press is a global override: starts a session from anywhere,
  // or ends one if already active. Checked first, every loop, regardless
  // of state — this guarantees manual control always works even if the
  // automatic wake-word/session logic gets stuck.
  bool pressed = buttonWasPressed();
  if (pressed) {
    if (_botState == STATE_IDLE_LISTENING || _botState == STATE_WAKE_CHECKING) {
      Serial.println("[main] Button pressed -> starting session manually");
      _botState = STATE_SESSION_LISTENING;
      _lastSpeechDetectedAt = millis();
      faceSetState(FACE_LISTENING);
      return; // skip rest of this loop iteration, start clean next pass
    } else {
      endSession("button pressed during active session");
      return;
    }
  }

  switch (_botState) {

    case STATE_IDLE_LISTENING: {
      faceSetState(FACE_IDLE);
      if (micHasRecentSound(0.5)) {
        Serial.println("[main] Sound detected while idle -> checking for wake word");
        _botState = STATE_WAKE_CHECKING;
      }
      break;
    }

    case STATE_WAKE_CHECKING: {
      // Grab a short chunk and ask the PC if the wake word was in it.
      size_t n = 0;
      int16_t* chunk = micGetLastSeconds(WAKE_CHUNK_SECONDS, &n);
      if (!chunk || n == 0) { _botState = STATE_IDLE_LISTENING; break; }

      if (!transportIsConnected() && !transportReconnect()) {
        free(chunk);
        Serial.println("[main] Network down during wake check -> error state");
        _botState = STATE_ERROR;
        _errorStateEnteredAt = millis();
        break;
      }

      transportSendAudioTyped(MSG_TYPE_WAKE_CHECK, chunk, n);
      free(chunk);

      uint8_t respType = 0;
      size_t respLen = 0;
      uint8_t* resp = transportReceiveTyped(&respType, &respLen, 5000);

      bool wakeDetected = false;
      if (resp && respType == RESP_TYPE_WAKE_RESULT && respLen >= 1) {
        wakeDetected = (resp[0] == 0x01);
      }
      if (resp) free(resp);

      if (wakeDetected) {
        Serial.println("[main] Wake word detected -> starting session");
        _botState = STATE_SESSION_LISTENING;
        _lastSpeechDetectedAt = millis();
        faceSetState(FACE_LISTENING);
      } else {
        // Not the wake word — just ambient/other sound. Back to idle.
        _botState = STATE_IDLE_LISTENING;
      }
      break;
    }

    case STATE_SESSION_LISTENING: {
      faceSetState(FACE_LISTENING);

      bool soundNow = micHasRecentSound(0.5);

      if (soundNow) {
        _lastSpeechDetectedAt = millis();
        if (!_capturingUtterance) {
          _capturingUtterance = true;
          _sessionUtteranceStartedAt = millis();
        }
        // Cap max utterance length so a stuck-open mic can't buffer forever
        if ((millis() - _sessionUtteranceStartedAt) > (SESSION_MAX_UTTERANCE_SEC * 1000UL)) {
          Serial.println("[main] Max utterance length reached -> sending");
          _botState = STATE_SESSION_SENDING;
        }
      } else if (_capturingUtterance) {
        // Sound just stopped after having spoken — treat as end of utterance
        // once there's been a brief pause, so we don't cut off mid-word.
        if ((millis() - _lastSpeechDetectedAt) > 700) {
          Serial.println("[main] Utterance finished -> sending");
          _botState = STATE_SESSION_SENDING;
        }
      }

      // Auto-end session on prolonged silence (no one has spoken in a while)
      if (!_capturingUtterance && (millis() - _lastSpeechDetectedAt) > SESSION_SILENCE_TIMEOUT_MS) {
        endSession("silence timeout");
      }
      break;
    }

    case STATE_SESSION_SENDING: {
      faceSetState(FACE_THINKING);
      _capturingUtterance = false;

      // Grab the utterance length actually spoken, capped at buffer size.
      float secondsToGrab = min((float)((millis() - _sessionUtteranceStartedAt) / 1000.0 + 0.5),
                                 (float)RECORD_SECONDS);
      size_t n = 0;
      int16_t* audio = micGetLastSeconds(secondsToGrab, &n);

      if (!audio || n == 0) {
        Serial.println("[main] No audio captured for utterance -> back to session listening");
        if (audio) free(audio);
        _botState = STATE_SESSION_LISTENING;
        _lastSpeechDetectedAt = millis();
        break;
      }

      if (!transportIsConnected() && !transportReconnect()) {
        free(audio);
        Serial.println("[main] Network down -> error state");
        _botState = STATE_ERROR;
        _errorStateEnteredAt = millis();
        break;
      }

      Serial.printf("[main] Sending utterance: %.2f sec\n", (float)n / SAMPLE_RATE_HZ);
      transportSendAudioTyped(MSG_TYPE_CONVERSATION, audio, n);
      free(audio);

      _botState = STATE_SESSION_WAITING;
      break;
    }

    case STATE_SESSION_WAITING: {
      faceSetState(FACE_THINKING);

      uint8_t respType = 0;
      size_t respLen = 0;
      uint8_t* resp = transportReceiveTyped(&respType, &respLen, 30000);

      if (!resp || respType != RESP_TYPE_AUDIO_REPLY || respLen == 0) {
        Serial.println("[main] No usable reply -> back to session listening");
        if (resp) free(resp);
        _botState = STATE_SESSION_LISTENING;
        _lastSpeechDetectedAt = millis(); // don't instantly re-timeout
        break;
      }

      Serial.printf("[main] Got reply: %u bytes -> playing\n", (unsigned)respLen);
      faceSetState(FACE_TALKING);
      speakerPlayPCM((int16_t*)resp, respLen / sizeof(int16_t));
      free(resp);

      _botState = STATE_SESSION_LISTENING;
      _lastSpeechDetectedAt = millis(); // reset silence timer after bot finishes talking
      Serial.println("[main] Reply finished. Listening for next turn.");
      break;
    }

    case STATE_ERROR: {
      faceSetState(FACE_ERROR);
      if (millis() - _errorStateEnteredAt > 5000) {
        Serial.println("[main] Retrying network connection...");
        if (transportReconnect()) {
          Serial.println("[main] Recovered. Back to idle.");
          _botState = STATE_IDLE_LISTENING;
        } else {
          _errorStateEnteredAt = millis();
        }
      }
      break;
    }
  }
}

/*
  ============================================================================
  STANDALONE MODULE TESTS — copy the relevant snippet into a bare sketch
  (with the same config.h/mic.h/etc files alongside it) to test ONE piece
  in isolation before running the full integrated sketch.
  ============================================================================

  --- Test mic + VAD only ---
  #include "config.h"
  #include "mic.h"
  void setup() { Serial.begin(115200); micInit(); }
  void loop() {
    micTask();
    Serial.println(micHasRecentSound(0.5) ? "SOUND" : "quiet");
    delay(200);
  }

  --- Test speaker only ---
  #include "config.h"
  #include "speaker.h"
  void setup() { Serial.begin(115200); speakerInit(); }
  void loop() { speakerTestTone(1000); delay(2000); }

  --- Test all 3 displays only ---
  #include "config.h"
  #include "face.h"
  void setup() { Serial.begin(115200); faceInit(); }
  void loop() {
    faceSetState(FACE_IDLE);      for(int i=0;i<50;i++){faceUpdate();delay(50);}
    faceSetState(FACE_LISTENING); for(int i=0;i<20;i++){faceUpdate();delay(50);}
    faceSetState(FACE_THINKING);  for(int i=0;i<20;i++){faceUpdate();delay(50);}
    faceSetState(FACE_TALKING);   for(int i=0;i<20;i++){faceUpdate();delay(50);}
  }

  --- Test button only ---
  #include "config.h"
  #include "button.h"
  void setup() { Serial.begin(115200); buttonInit(); }
  void loop() { if (buttonWasPressed()) Serial.println("PRESSED"); }

  --- Test network + wake protocol only (run bridge.py first) ---
  #include "config.h"
  #include "network.h"
  void setup() {
    Serial.begin(115200);
    transportInit();
    int16_t testBuf[1600];
    memset(testBuf, 0, sizeof(testBuf));
    transportSendAudioTyped(MSG_TYPE_WAKE_CHECK, testBuf, 1600);
    uint8_t respType; size_t respLen;
    uint8_t* resp = transportReceiveTyped(&respType, &respLen, 5000);
    Serial.println(resp ? "Got response!" : "No response / timeout");
  }
  void loop() {}
  ============================================================================
*/
