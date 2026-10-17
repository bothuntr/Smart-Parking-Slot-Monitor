// Pico 2 W slot indicator - LEDs only (no buzzer)
// Board: "Raspberry Pi Pico 2 W" (arduino-pico core by Earle Philhower)
//
// Wiring:
//   ESP32 GPIO23 -> Pico GP15   (HIGH = slot occupied)
//   ESP32 GND    -> Pico GND    (common ground is required)
//   GP14 -> 220 ohm -> RED LED   -> GND
//   GP13 -> 220 ohm -> GREEN LED -> GND
//
// Green = slot free, Red = slot occupied.

const int IN_PIN = 15;
const int RED    = 14;
const int GREEN  = 13;

bool lastState = false;

void show(bool occupied) {
  digitalWrite(RED, occupied);
  digitalWrite(GREEN, !occupied);
}

void setup() {
  pinMode(IN_PIN, INPUT_PULLDOWN);   // reads LOW if the wire is disconnected
  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);

  lastState = digitalRead(IN_PIN);
  show(lastState);
}

void loop() {
  bool s = digitalRead(IN_PIN);

  if (s != lastState) {
    delay(50);                       // debounce
    s = digitalRead(IN_PIN);
    if (s != lastState) {
      lastState = s;
      show(s);
    }
  }

  delay(10);
}
