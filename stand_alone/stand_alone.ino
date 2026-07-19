  #include "config.h"
  #include "mic.h"
  void setup() { Serial.begin(115200); micInit(); }
  void loop() {
    micTask();
    Serial.println(micHasRecentSound(0.5) ? "SOUND" : "quiet");
    delay(200);
  }