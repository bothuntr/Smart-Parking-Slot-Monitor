
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

// =====================================================
// 1. CONFIGURATION
// =====================================================

// Telegram credentials. Keep these as placeholders in Git and enter the real
// values in the setup portal instead (press 'c' in the Serial Monitor).
const char* BOT_TOKEN = "PASTE_YOUR_NEW_BOT_TOKEN_HERE";
const char* CHAT_ID   = "PASTE_YOUR_CHAT_ID_HERE";

// WiFiManager access point
const char* AP_NAME = "Parking-Setup";
const char* AP_PASSWORD = "change-this-password";   // setup portal password (min. 8 characters)

// Hardware pins
static constexpr uint8_t TRIG_PIN = 5;
static constexpr uint8_t ECHO_PIN = 18;
static constexpr uint8_t PICO_PIN = 23;

// Default parking settings
static constexpr float DEFAULT_CAR_CM = 10.0f;
static constexpr float DEFAULT_CLEAR_CM = 15.0f;
static constexpr uint32_t DEFAULT_CONFIRM_MS = 5000;
static constexpr uint32_t DEFAULT_RATE = 20;

// India Standard Time
static constexpr long GMT_OFFSET_SEC = 19800;
static constexpr int DST_OFFSET_SEC = 0;

// Timing
static constexpr uint32_t SENSOR_INTERVAL_MS = 200;
static constexpr uint32_t LOG_INTERVAL_MS = 2000;
static constexpr uint32_t WIFI_RETRY_MS = 10000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 8000;

// =====================================================
// 2. CONFIGURATION STORAGE
// =====================================================

struct ParkingConfig {
  String slot;
  String userName;
  String botToken;
  String chatId;

  float carCm;
  float clearCm;

  uint32_t confirmMs;
  uint32_t ratePerHour;
};

ParkingConfig cfg;
Preferences prefs;

// =====================================================
// 3. RUNTIME STATE
// =====================================================

bool parked = false;
bool lastRaw = false;
bool sensorInitialized = false;
bool lastDistanceValid = false;

float lastDistanceCm = NAN;

uint32_t lastSensorRead = 0;
uint32_t lastDistanceLog = 0;
uint32_t lastWiFiRetry = 0;
uint32_t changeStart = 0;

time_t entryTime = 0;
uint32_t ticketNo = 1000;

// =====================================================
// 4. CONFIGURATION LOAD/SAVE
// =====================================================

void loadConfig() {
  cfg.slot = prefs.getString("slot", "A1");
  cfg.userName = prefs.getString("user", "Your Name");

  cfg.botToken = prefs.getString("bot", "");
  cfg.chatId = prefs.getString("chat", CHAT_ID);

  cfg.carCm = prefs.getFloat("carcm", DEFAULT_CAR_CM);
  cfg.clearCm = prefs.getFloat("clearcm", DEFAULT_CLEAR_CM);

  cfg.confirmMs = prefs.getUInt("confirm", DEFAULT_CONFIRM_MS);
  cfg.ratePerHour = prefs.getUInt("rate", DEFAULT_RATE);

  // Use the token from the source code if storage is empty.
  // This also fixes older firmware configurations containing
  // an empty saved bot token.
  if (cfg.botToken.length() == 0 ||
      cfg.botToken == "PASTE_YOUR_NEW_BOT_TOKEN_HERE") {
    cfg.botToken = String(BOT_TOKEN);
  }

  if (cfg.chatId.length() == 0) {
    cfg.chatId = String(CHAT_ID);
  }
}

void saveConfig() {
  prefs.putString("slot", cfg.slot);
  prefs.putString("user", cfg.userName);
  prefs.putString("bot", cfg.botToken);
  prefs.putString("chat", cfg.chatId);

  prefs.putFloat("carcm", cfg.carCm);
  prefs.putFloat("clearcm", cfg.clearCm);

  prefs.putUInt("confirm", cfg.confirmMs);
  prefs.putUInt("rate", cfg.ratePerHour);
}

// =====================================================
// 5. WIFI MANAGER + SETTINGS PORTAL
// =====================================================

bool configureWiFi(bool forcePortal) {
  WiFiManager wm;

  wm.setTitle("Parking System Setup");
  wm.setHostname("parking-esp32");
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(0);
  wm.setWiFiAutoReconnect(true);

  char slotBuf[16];
  char userBuf[40];
  char botBuf[160];
  char chatBuf[32];
  char carBuf[16];
  char clearBuf[16];
  char confirmBuf[16];
  char rateBuf[16];

  snprintf(slotBuf, sizeof(slotBuf), "%s", cfg.slot.c_str());
  snprintf(userBuf, sizeof(userBuf), "%s", cfg.userName.c_str());
  snprintf(botBuf, sizeof(botBuf), "%s", cfg.botToken.c_str());
  snprintf(chatBuf, sizeof(chatBuf), "%s", cfg.chatId.c_str());

  snprintf(carBuf, sizeof(carBuf), "%.2f", cfg.carCm);
  snprintf(clearBuf, sizeof(clearBuf), "%.2f", cfg.clearCm);

  snprintf(confirmBuf, sizeof(confirmBuf), "%lu",
           (unsigned long)cfg.confirmMs);

  snprintf(rateBuf, sizeof(rateBuf), "%lu",
           (unsigned long)cfg.ratePerHour);

  WiFiManagerParameter pSlot(
    "slot", "Parking slot", slotBuf, 15);

  WiFiManagerParameter pUser(
    "username", "User name", userBuf, 39);

  WiFiManagerParameter pBot(
    "bottoken", "Telegram bot token", botBuf, 159);

  WiFiManagerParameter pChat(
    "chatid", "Telegram chat ID", chatBuf, 31);

  WiFiManagerParameter pCar(
    "carcm", "Arrival threshold (cm)", carBuf, 15);

  WiFiManagerParameter pClear(
    "clearcm", "Departure threshold (cm)", clearBuf, 15);

  WiFiManagerParameter pConfirm(
    "confirm", "Confirmation time (ms)", confirmBuf, 15);

  WiFiManagerParameter pRate(
    "rate", "Fare per started hour (INR)", rateBuf, 15);

  wm.addParameter(&pSlot);
  wm.addParameter(&pUser);
  wm.addParameter(&pBot);
  wm.addParameter(&pChat);
  wm.addParameter(&pCar);
  wm.addParameter(&pClear);
  wm.addParameter(&pConfirm);
  wm.addParameter(&pRate);

  Serial.println();
  Serial.println("[WIFI] Starting configuration...");

  bool connected;

  if (forcePortal) {
    connected = wm.startConfigPortal(AP_NAME, AP_PASSWORD);
  } else {
    connected = wm.autoConnect(AP_NAME, AP_PASSWORD);
  }

  if (!connected) {
    Serial.println("[WIFI] Connection/configuration failed.");
    return false;
  }

  // Read settings submitted through the portal.
  cfg.slot = String(pSlot.getValue());
  cfg.userName = String(pUser.getValue());
  cfg.botToken = String(pBot.getValue());
  cfg.chatId = String(pChat.getValue());

  cfg.slot.trim();
  cfg.userName.trim();
  cfg.botToken.trim();
  cfg.chatId.trim();

  // A blank portal field must not erase the configured token.
  if (cfg.botToken.length() == 0) {
    cfg.botToken = String(BOT_TOKEN);
  }

  if (cfg.chatId.length() == 0) {
    cfg.chatId = String(CHAT_ID);
  }

  float car = atof(pCar.getValue());
  float clear = atof(pClear.getValue());

  unsigned long confirm =
    strtoul(pConfirm.getValue(), nullptr, 10);

  unsigned long rate =
    strtoul(pRate.getValue(), nullptr, 10);

  // Validate submitted configuration.
  if (cfg.slot.length() == 0 ||
      cfg.userName.length() == 0) {
    Serial.println("[CONFIG] Slot/user name cannot be empty.");
    return false;
  }

  if (!isfinite(car) || !isfinite(clear) ||
      car < 2.0f || car > 350.0f ||
      clear <= car || clear > 400.0f) {
    Serial.println("[CONFIG] Invalid sensor thresholds.");
    return false;
  }

  if (confirm < 500 || confirm > 60000 ||
      rate > 1000000UL) {
    Serial.println("[CONFIG] Invalid confirmation/fare.");
    return false;
  }

  cfg.carCm = car;
  cfg.clearCm = clear;
  cfg.confirmMs = confirm;
  cfg.ratePerHour = rate;

  saveConfig();

  Serial.println("[CONFIG] Saved successfully.");
  Serial.printf("[CONFIG] Bot token length: %u\n",
                (unsigned)cfg.botToken.length());
  Serial.printf("[CONFIG] Chat ID length: %u\n",
                (unsigned)cfg.chatId.length());

  Serial.print("[WIFI] Connected SSID: ");
  Serial.println(WiFi.SSID());

  Serial.print("[WIFI] IP address: ");
  Serial.println(WiFi.localIP());

  Serial.println("[TELEGRAM] Credentials configured: " +
    String(cfg.botToken.length() > 0 &&
           cfg.chatId.length() > 0 ? "YES" : "NO"));

  return true;
}

// =====================================================
// 6. TIME MANAGEMENT
// =====================================================

bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

void initializeTime() {
  configTime(
    GMT_OFFSET_SEC,
    DST_OFFSET_SEC,
    "pool.ntp.org",
    "time.google.com"
  );
}

String fmtTime(time_t t) {
  if (t <= 0) {
    return "Time unavailable";
  }

  struct tm tmv;

  if (!localtime_r(&t, &tmv)) {
    return "Time unavailable";
  }

  char buffer[32];

  strftime(
    buffer, sizeof(buffer),
    "%d %b %Y %H:%M:%S", &tmv
  );

  return String(buffer);
}

String fmtDuration(uint64_t seconds) {
  if (seconds < 60) {
    return String((unsigned long)seconds) + " s";
  }

  if (seconds < 3600) {
    return String((unsigned long)(seconds / 60)) +
           " min " +
           String((unsigned long)(seconds % 60)) + " s";
  }

  return String((unsigned long)(seconds / 3600)) +
         " h " +
         String((unsigned long)((seconds % 3600) / 60)) +
         " min " +
         String((unsigned long)(seconds % 60)) + " s";
}

// =====================================================
// 7. TELEGRAM MESSAGING
// =====================================================

bool sendTelegram(const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TELEGRAM] ERROR: WiFi disconnected.");
    return false;
  }

  if (cfg.botToken.length() == 0 ||
      cfg.chatId.length() == 0) {
    Serial.println("[TELEGRAM] ERROR: Missing bot token/chat ID.");
    Serial.println("[TELEGRAM] Check BOT_TOKEN and CHAT_ID.");
    return false;
  }

  if (cfg.botToken == "PASTE_YOUR_NEW_BOT_TOKEN_HERE") {
    Serial.println("[TELEGRAM] ERROR: Replace the token placeholder.");
    return false;
  }

  WiFiClientSecure client;

  // Prototype only: use certificate verification in production.
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;

  String url =
    "https://api.telegram.org/bot" +
    cfg.botToken +
    "/sendMessage";

  if (!http.begin(client, url)) {
    Serial.println("[TELEGRAM] ERROR: HTTPS initialization failed.");
    return false;
  }

  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;

  doc["chat_id"] = cfg.chatId;
  doc["text"] = message;

  String body;
  serializeJson(doc, body);

  Serial.println("[TELEGRAM] Sending notification...");

  int status = http.POST(body);
  bool success = false;

  if (status >= 200 && status < 300) {
    String responseBody = http.getString();

    JsonDocument response;

    if (deserializeJson(response, responseBody) ==
        DeserializationError::Ok) {
      success = response["ok"] | false;
    }
  } else if (status < 0) {
    Serial.print("[TELEGRAM] Network error: ");
    Serial.println(http.errorToString(status));
  } else {
    Serial.print("[TELEGRAM] HTTP error: ");
    Serial.println(status);
    Serial.println(http.getString());
  }

  http.end();

  Serial.println(success
    ? "[TELEGRAM] Notification sent successfully."
    : "[TELEGRAM] Notification failed.");

  return success;
}

// =====================================================
// 8. ULTRASONIC SENSOR
// =====================================================

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
    pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0) {
    return NAN;
  }

  return duration * 0.0343f / 2.0f;
}

// =====================================================
// 9. PARKING STATE STORAGE
// =====================================================

void setOccupied(bool occupied) {
  parked = occupied;

  digitalWrite(
    PICO_PIN,
    occupied ? HIGH : LOW
  );

  prefs.putBool("occupied", occupied);
}

void restoreParkingState() {
  ticketNo = prefs.getUInt("ticket", 1000);
  parked = prefs.getBool("occupied", false);

  uint64_t savedEntry = prefs.getULong64("entry", 0);
  entryTime = (time_t)savedEntry;

  digitalWrite(PICO_PIN, parked ? HIGH : LOW);

  Serial.printf("[PARKING] Ticket counter: %lu\n",
                (unsigned long)ticketNo);

  Serial.println(parked
    ? "[PARKING] Restored status: OCCUPIED"
    : "[PARKING] Restored status: FREE");
}

// =====================================================
// 10. VEHICLE ARRIVAL
// =====================================================

void handleArrival() {
  setOccupied(true);

  entryTime = time(nullptr);

  prefs.putULong64("entry", (uint64_t)entryTime);

  ticketNo++;
  prefs.putUInt("ticket", ticketNo);

  Serial.println("[PARKING] Vehicle arrival confirmed.");

  String message =
    "PARKING TICKET #" + String(ticketNo) +
    "\nName: " + cfg.userName +
    "\nSlot: " + cfg.slot +
    "\nEntry: " + fmtTime(entryTime) +
    "\nFare: INR " + String(cfg.ratePerHour) +
    " per started hour";

  sendTelegram(message);
}

// =====================================================
// 11. VEHICLE DEPARTURE + BILLING
// =====================================================

void handleDeparture() {
  setOccupied(false);

  time_t exitTime = time(nullptr);

  uint64_t seconds = 0;

  if (entryTime > 0 && exitTime >= entryTime) {
    seconds = (uint64_t)(exitTime - entryTime);
  }

  // Charge for each started hour; minimum one hour.
  uint64_t hours = (seconds + 3599ULL) / 3600ULL;

  if (hours < 1) {
    hours = 1;
  }

  uint64_t fee = hours * (uint64_t)cfg.ratePerHour;

  Serial.println("[PARKING] Vehicle departure confirmed.");

  String message =
    "PARKING RECEIPT - Ticket #" + String(ticketNo) +
    "\nName: " + cfg.userName +
    "\nSlot: " + cfg.slot +
    "\nEntry: " + fmtTime(entryTime) +
    "\nExit: " + fmtTime(exitTime) +
    "\nDuration: " + fmtDuration(seconds) +
    "\nBillable hours: " + String((unsigned long)hours) +
    "\nFee: INR " + String((unsigned long)fee);

  sendTelegram(message);

  entryTime = 0;
  prefs.putULong64("entry", 0);
}

// =====================================================
// 12. SENSOR PROCESSING + 2-SECOND DIAGNOSTICS
// =====================================================

void updateParkingDetection() {
  const uint32_t now = millis();

  // Read sensor at the normal polling interval.
  if ((uint32_t)(now - lastSensorRead) >=
      SENSOR_INTERVAL_MS) {

    lastSensorRead = now;

    float distance = readDistanceCm();

    if (isfinite(distance)) {
      lastDistanceCm = distance;
      lastDistanceValid = true;

      bool rawOccupied = parked
        ? (distance < cfg.clearCm)
        : (distance < cfg.carCm);

      if (!sensorInitialized) {
        sensorInitialized = true;
        lastRaw = rawOccupied;
        changeStart = now;
      } else {
        if (rawOccupied != lastRaw) {
          lastRaw = rawOccupied;
          changeStart = now;
        }

        if ((uint32_t)(now - changeStart) >=
            cfg.confirmMs) {

          if (rawOccupied && !parked) {
            handleArrival();
          } else if (!rawOccupied && parked) {
            handleDeparture();
          }
        }
      }
    } else {
      // A timeout is not evidence that the slot is empty.
      lastDistanceValid = false;
    }
  }

  // Print the latest sensor reading every two seconds.
  if ((uint32_t)(now - lastDistanceLog) >=
      LOG_INTERVAL_MS) {

    lastDistanceLog = now;

    Serial.print("[ULTRASONIC] Distance: ");

    if (lastDistanceValid) {
      Serial.print(lastDistanceCm, 2);
      Serial.print(" cm");
    } else {
      Serial.print("INVALID (no echo/timeout)");
    }

    Serial.print(" | Parking: ");
    Serial.print(parked ? "OCCUPIED" : "FREE");

    Serial.print(" | WiFi: ");
    Serial.println(
      WiFi.status() == WL_CONNECTED
        ? "CONNECTED"
        : "DISCONNECTED"
    );
  }
}

// =====================================================
// 13. SERIAL COMMANDS
// =====================================================

void handleSerialCommands() {
  if (!Serial.available()) {
    return;
  }

  char command = Serial.read();

  if (command == 'c' || command == 'C') {
    Serial.println("[CONFIG] Opening setup portal...");

    if (configureWiFi(true)) {
      Serial.println("[CONFIG] Settings updated.");
    } else {
      Serial.println("[CONFIG] Portal failed or settings invalid.");
    }

    sensorInitialized = false;
    lastSensorRead = millis();
  }

  if (command == 's' || command == 'S') {
    Serial.println("----- PARKING CONFIGURATION -----");

    Serial.println("Slot: " + cfg.slot);
    Serial.println("User: " + cfg.userName);

    Serial.printf("Arrival threshold: %.2f cm\n", cfg.carCm);
    Serial.printf("Departure threshold: %.2f cm\n", cfg.clearCm);

    Serial.printf("Confirmation: %lu ms\n",
                  (unsigned long)cfg.confirmMs);

    Serial.printf("Fare: INR %lu/hour\n",
                  (unsigned long)cfg.ratePerHour);

    Serial.printf("Bot token length: %u\n",
                  (unsigned)cfg.botToken.length());

    Serial.printf("Chat ID length: %u\n",
                  (unsigned)cfg.chatId.length());

    Serial.println(parked
      ? "Status: OCCUPIED"
      : "Status: FREE");

    Serial.println("---------------------------------");
  }
}

// =====================================================
// 14. SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PICO_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(PICO_PIN, LOW);

  prefs.begin("parking", false);

  loadConfig();
  restoreParkingState();

  Serial.println();
  Serial.println("==================================");
  Serial.println("       PARKING SYSTEM START");
  Serial.println("==================================");

  Serial.println("Serial commands:");
  Serial.println("  c = Open configuration portal");
  Serial.println("  s = Show current settings");

  if (!configureWiFi(false)) {
    Serial.println("[SYSTEM] WiFi setup failed; restarting.");
    delay(5000);
    ESP.restart();
  }

  initializeTime();

  Serial.println("[SYSTEM] Parking system ready.");

  Serial.println(cfg.botToken.length() > 0 &&
                 cfg.botToken != "PASTE_YOUR_NEW_BOT_TOKEN_HERE"
    ? "[TELEGRAM] Bot token available."
    : "[TELEGRAM] Bot token needs configuration.");

  Serial.println("[SYSTEM] Distance logging interval: 2 seconds.");
}

// =====================================================
// 15. MAIN LOOP
// =====================================================

void loop() {
  handleSerialCommands();

  if (WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();

    if ((uint32_t)(now - lastWiFiRetry) >= WIFI_RETRY_MS) {
      lastWiFiRetry = now;

      Serial.println("[WIFI] Reconnecting...");
      WiFi.reconnect();
    }
  }

  updateParkingDetection();

  delay(2);
}
