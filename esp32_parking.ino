#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>

// ---------- EDIT THESE ----------
const char* ssid      = "YOUR_WIFI_NAME";          // 2.4 GHz network
const char* pass      = "YOUR_WIFI_PASSWORD";
const char* BOT_TOKEN = "YOUR_NEW_BOT_TOKEN";
const char* CHAT_ID   = "YOUR_CHAT_ID";
const char* SLOT      = "A1";
const char* USER_NAME = "Your Name";       // shown in every ticket and receipt

const float CAR_CM   = 10.0;             // closer than this = car arrives
const float CLEAR_CM = 15.0;             // farther than this = car leaves (hysteresis)
const unsigned long CONFIRM_MS = 5000;   // must hold this long to count
const int RATE_PER_HOUR = 20;            // fee per started hour
const long GMT_OFFSET_SEC = 19800;       // UTC+5:30
// --------------------------------

const int TRIG = 5, ECHO = 18;
const int OCC_PIN = 23;   // signal line to the Pico 2 W (HIGH = occupied)

Preferences prefs;
bool parked = false, lastRaw = false;
unsigned long changeStart = 0;
time_t entryTime = 0;
unsigned int ticketNo = 1000;

float readCm() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long t = pulseIn(ECHO, HIGH, 30000);
  return t == 0 ? 400 : t * 0.0343 / 2;
}

String fmt(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[32];
  strftime(b, sizeof(b), "%d %b %H:%M:%S", &tmv);
  return String(b);
}

// 24 -> "24 s", 135 -> "2 min 15 s", 3700 -> "1 h 1 min 40 s"
String fmtDuration(unsigned long secs) {
  if (secs < 60) return String(secs) + " s";
  if (secs < 3600) return String(secs / 60) + " min " + String(secs % 60) + " s";
  return String(secs / 3600) + " h " + String((secs % 3600) / 60) + " min " +
         String(secs % 60) + " s";
}

void sendTelegram(String text) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage");
  http.addHeader("Content-Type", "application/json");
  http.POST(String("{\"chat_id\":\"") + CHAT_ID + "\",\"text\":\"" + text + "\"}");
  http.end();
}

void setup() {
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(OCC_PIN, OUTPUT);
  digitalWrite(OCC_PIN, LOW);

  prefs.begin("parking", false);               // namespace "parking", read/write
  ticketNo = prefs.getUInt("ticket", 1000);    // 1000 if nothing saved yet

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org");
  while (time(nullptr) < 100000) delay(200);
}

void loop() {
  float d = readCm();

  // Hysteresis: different thresholds for entering and leaving
  bool raw = parked ? (d < CLEAR_CM) : (d < CAR_CM);

  if (raw != lastRaw) {
    lastRaw = raw;
    changeStart = millis();
  }

  if (millis() - changeStart > CONFIRM_MS) {
    if (raw && !parked) {
      parked = true;
      digitalWrite(OCC_PIN, HIGH);             // Pico: red LED + beep, instantly
      entryTime = time(nullptr);
      ticketNo++;
      prefs.putUInt("ticket", ticketNo);       // one flash write per ticket
      sendTelegram("PARKING TICKET #" + String(ticketNo) +
                   "\\nName: " + USER_NAME +
                   "\\nSlot: " + SLOT +
                   "\\nEntry: " + fmt(entryTime));
    }
    if (!raw && parked) {
      parked = false;
      digitalWrite(OCC_PIN, LOW);              // Pico: green LED + double beep
      time_t now = time(nullptr);
      unsigned long secs = now - entryTime;
      unsigned long hours = (secs + 3599) / 3600;   // started hours
      if (hours < 1) hours = 1;
      sendTelegram("EXIT - Ticket #" + String(ticketNo) +
                   "\\nName: " + USER_NAME +
                   "\\nSlot: " + SLOT +
                   "\\nEntry: " + fmt(entryTime) +
                   "\\nExit: " + fmt(now) +
                   "\\nDuration: " + fmtDuration(secs) +
                   "\\nFee: " + String(hours * RATE_PER_HOUR));
    }
  }

  delay(200);
}
