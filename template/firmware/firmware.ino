// Your firmware. Change anything here, commit, and your boards update themselves.
//
// The OTA updater lives in its own part of the flash: no code you write here can
// break or remove it. The two OTAForever lines below are optional extras:
//   connectWiFi() - use the WiFi you entered on the setup page
//   begin(5)      - check for new builds every 5 minutes while running
//                   (without it, boards update on every restart)

#include <OTAForever.h>

#ifdef LED_BUILTIN
const int LED_PIN = LED_BUILTIN;  // the board's own LED, if the board defines one
#else
const int LED_PIN = 2;            // change to the pin your LED is on
#endif

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  if (OTAForever.connectWiFi()) {
    Serial.printf("WiFi connected. Running build #%lu from %s\n",
                  (unsigned long)OTAForever.installedBuild(), OTAForever.repo().c_str());
    OTAForever.begin(5);
  }
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  delay(500);
}
