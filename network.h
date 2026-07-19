/*
  ============================================================================
  network.h — Transport layer to the PC bridge script.
  Provides ONE interface regardless of whether TRANSPORT_MODE is WiFi or
  Serial — main.ino never needs to know which is active. This is the key to
  your "flip one line if WiFi fails" safety net.

  PROTOCOL (same over WiFi or Serial), now with a message TYPE byte so the
  PC bridge can tell a wake-word check apart from a real conversation turn:

    Outgoing (ESP32 -> PC):
      [1 byte: message type][4 bytes: uint32 audio length][raw PCM16 audio]
      message type: 0x01 = WAKE_CHECK   (short chunk, asking "was my name said?")
                    0x02 = CONVERSATION  (full utterance, wants a real reply)

    Incoming (PC -> ESP32):
      [1 byte: response type][4 bytes: uint32 payload length][payload]
      response type: 0x10 = WAKE_RESULT  (payload = 1 byte: 0x00 no / 0x01 yes)
                     0x11 = AUDIO_REPLY  (payload = raw PCM16 audio to play)

  Simple, fixed-header framing — easy to debug by eye in a hex dump if needed.
  ============================================================================
*/
#pragma once
#include "config.h"

#define MSG_TYPE_WAKE_CHECK    0x01
#define MSG_TYPE_CONVERSATION  0x02
#define RESP_TYPE_WAKE_RESULT  0x10
#define RESP_TYPE_AUDIO_REPLY  0x11

#if TRANSPORT_MODE == TRANSPORT_WIFI
  #include <WiFi.h>
  static WiFiClient _client;
  static bool _wifiConnected = false;
#endif

inline bool transportInit() {
#if TRANSPORT_MODE == TRANSPORT_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[network] Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[network] WiFi FAILED to connect within timeout");
    return false;
  }
  Serial.print("[network] WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  Serial.printf("[network] Connecting to PC bridge at %s:%d...\n", PC_SERVER_IP, PC_SERVER_PORT);
  if (!_client.connect(PC_SERVER_IP, PC_SERVER_PORT)) {
    Serial.println("[network] FAILED to connect to PC bridge server. Is bridge.py running?");
    return false;
  }
  _wifiConnected = true;
  Serial.println("[network] Connected to PC bridge OK");
  return true;

#elif TRANSPORT_MODE == TRANSPORT_SERIAL
  Serial.println("[network] Using USB Serial transport (make sure bridge.py is set to serial mode)");
  return true;
#endif
}

inline bool transportIsConnected() {
#if TRANSPORT_MODE == TRANSPORT_WIFI
  return _wifiConnected && _client.connected();
#else
  return true;
#endif
}

inline bool transportReconnect() {
#if TRANSPORT_MODE == TRANSPORT_WIFI
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(300);
    }
    if (WiFi.status() != WL_CONNECTED) return false;
  }
  _client.stop();
  _wifiConnected = _client.connect(PC_SERVER_IP, PC_SERVER_PORT);
  return _wifiConnected;
#else
  return true;
#endif
}

#if TRANSPORT_MODE == TRANSPORT_WIFI
  #define _TRANSPORT_STREAM (&_client)
#else
  #define _TRANSPORT_STREAM (&Serial)
#endif

// Sends raw PCM audio tagged with a message type (wake-check or conversation).
inline bool transportSendAudioTyped(uint8_t msgType, const int16_t* samples, size_t sampleCount) {
  uint32_t byteLen = sampleCount * sizeof(int16_t);

#if TRANSPORT_MODE == TRANSPORT_WIFI
  if (!transportIsConnected()) return false;
#endif

  Stream* s = _TRANSPORT_STREAM;
  s->write(&msgType, 1);
  s->write((uint8_t*)&byteLen, 4);
  size_t sent = s->write((uint8_t*)samples, byteLen);

#if TRANSPORT_MODE == TRANSPORT_SERIAL
  Serial.flush();
#endif

  return sent == byteLen;
}

// Blocks until a full typed response is received (with a timeout).
// outRespType receives which response it was (WAKE_RESULT or AUDIO_REPLY).
// Returns a malloc'd buffer of the payload (caller must free()), or nullptr
// on timeout/error. For WAKE_RESULT, payload is 1 byte (0x00/0x01) —
// outSampleCount will be set to 0 in that case since it's not audio;
// check outRespType to know how to interpret the returned buffer.
inline uint8_t* transportReceiveTyped(uint8_t* outRespType, size_t* outByteLen, unsigned long timeoutMs = 30000) {
  Stream* stream = _TRANSPORT_STREAM;
  unsigned long start = millis();

  // Read 1-byte response type
  while (!stream->available()) {
    if (millis() - start > timeoutMs) { *outByteLen = 0; return nullptr; }
  }
  *outRespType = stream->read();

  // Read 4-byte length header
  uint8_t headerBuf[4];
  size_t headerReceived = 0;
  start = millis();
  while (headerReceived < 4) {
    if (millis() - start > timeoutMs) { *outByteLen = 0; return nullptr; }
    if (stream->available()) {
      headerBuf[headerReceived++] = stream->read();
    }
  }
  uint32_t byteLen;
  memcpy(&byteLen, headerBuf, 4);

  if (byteLen > 5000000) { // sanity guard against garbage/desync
    *outByteLen = 0;
    return nullptr;
  }
  if (byteLen == 0) {
    *outByteLen = 0;
    return nullptr;
  }

  uint8_t* buf = (uint8_t*)malloc(byteLen);
  if (!buf) { *outByteLen = 0; return nullptr; }

  size_t received = 0;
  start = millis();
  while (received < byteLen) {
    if (millis() - start > timeoutMs) { free(buf); *outByteLen = 0; return nullptr; }
    if (stream->available()) {
      int chunk = stream->readBytes((char*)buf + received, min((size_t)stream->available(), byteLen - received));
      received += chunk;
      start = millis(); // reset timeout on each successful chunk
    }
  }

  *outByteLen = byteLen;
  return buf;
}
