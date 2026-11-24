// Smart Parking Slot Monitor - ESP32 firmware
// ESP32 + HC-SR04 vehicle detection, RC522 RFID card identification,
// Telegram tickets and receipts, plus a web dashboard served by the ESP32
// (http://parking.local/). See README.md for wiring and setup.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// =====================================================
// 1. CONFIGURATION
// =====================================================

// Leave these as placeholders. Enter the real Telegram bot token and
// chat ID in the setup portal (Parking-Setup WiFi), so they never go into Git.
const char* BOT_TOKEN = "PASTE_YOUR_NEW_BOT_TOKEN_HERE";
const char* CHAT_ID   = "";

// WiFiManager access point
const char* AP_NAME = "Parking-Setup";
const char* AP_PASSWORD = "parksetup123";   // min. 8 characters

// Hardware pins
static constexpr uint8_t TRIG_PIN = 5;
static constexpr uint8_t ECHO_PIN = 18;
static constexpr uint8_t PICO_PIN = 23;

// RC522 RFID reader (SPI on custom pins)
static constexpr uint8_t RFID_SCK_PIN  = 25;
static constexpr uint8_t RFID_MISO_PIN = 26;
static constexpr uint8_t RFID_MOSI_PIN = 27;
static constexpr uint8_t RFID_SS_PIN   = 32;
static constexpr uint8_t RFID_RST_PIN  = 33;

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

// Departure is confirmed faster than arrival.
static constexpr uint32_t DEPART_CONFIRM_MS = 2000;

// HC-SR04 cannot measure below about 2 cm; smaller values are glitches.
static constexpr float MIN_VALID_CM = 2.0f;

// RFID
static constexpr uint32_t CARD_WINDOW_MS = 30000;  // time to tap after arrival
static constexpr uint32_t RFID_POLL_MS = 80;
static constexpr uint8_t MAX_CARDS = 10;

// Web dashboard
static const char* const DEFAULT_DASH_PIN = "7683";   // change it in the dashboard Settings
static constexpr uint32_t SCAN_WINDOW_MS = 30000;     // dashboard "Scan card" time limit

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

// RFID state
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
bool rfidOk = false;
bool pendingTicket = false;      // car parked, waiting for a card tap
uint32_t pendingStart = 0;
uint32_t lastRfidPoll = 0;
String sessionOwner = "";        // owner of the current parking session
String sessionCard = "";         // card UID of the current session
String sessionChat = "";         // Telegram chat ID of the session owner
bool sessionFree = false;       // registered card: parking is not charged
String guestChat = "";           // chat for unregistered cards and guests

// Web dashboard state
WebServer server(80);
String dashPin = DEFAULT_DASH_PIN;
bool scanActive = false;
uint32_t scanStart = 0;
String scanUid = "";

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

bool sendTelegramTo(const String& targetChat, const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TELEGRAM] ERROR: WiFi disconnected.");
    return false;
  }

  if (cfg.botToken.length() == 0 ||
      targetChat.length() == 0) {
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

  doc["chat_id"] = targetChat;
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

// Sends to the admin chat configured in the setup portal.
bool sendTelegram(const String& message) {
  return sendTelegramTo(cfg.chatId, message);
}

// Routes a ticket or receipt to one chat:
//   registered card  -> that card's chat (or the admin chat if none was set)
//   guest / unknown  -> the guest chat (set with the g command), else admin
// If delivery fails, the admin chat gets the message instead.
void routeMessage(const String& targetChat, const String& message) {
  String target = targetChat;

  if (target.length() == 0) {
    target = guestChat.length() > 0 ? guestChat : cfg.chatId;
  }

  bool ok = sendTelegramTo(target, message);

  if (!ok && target != cfg.chatId) {
    Serial.println("[TELEGRAM] Delivery failed; sending to the admin chat.");
    sendTelegram("[Not delivered to chat " + target + "]\n" + message);
  }
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

  sessionOwner = prefs.isKey("owner") ? prefs.getString("owner", "") : "";
  sessionCard = prefs.isKey("ownuid") ? prefs.getString("ownuid", "") : "";
  sessionChat = prefs.isKey("ownchat") ? prefs.getString("ownchat", "") : "";
  sessionFree = prefs.isKey("ownfree") && prefs.getBool("ownfree", false);
  guestChat = prefs.isKey("guestchat") ? prefs.getString("guestchat", "") : "";
  pendingTicket = parked && prefs.isKey("pend") && prefs.getBool("pend", false);
  pendingStart = millis();

  digitalWrite(PICO_PIN, parked ? HIGH : LOW);

  Serial.printf("[PARKING] Ticket counter: %lu\n",
                (unsigned long)ticketNo);

  Serial.println(parked
    ? "[PARKING] Restored status: OCCUPIED"
    : "[PARKING] Restored status: FREE");
}

// =====================================================
// 9B. RFID CARD REGISTRY (stored in flash / NVS)
// =====================================================

String uidToString(const MFRC522::Uid& uid) {
  String s;

  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) {
      s += "0";
    }
    s += String(uid.uidByte[i], HEX);
  }

  s.toUpperCase();
  return s;
}

// Returns true once per card tap.
bool readCardUid(String& uidOut) {
  if (!rfidOk) {
    return false;
  }

  if (!rfid.PICC_IsNewCardPresent()) {
    return false;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    return false;
  }

  uidOut = uidToString(rfid.uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  return true;
}

String cardField(const char* prefix, uint8_t index) {
  String key = String(prefix) + String(index);

  if (!prefs.isKey(key.c_str())) {
    return "";
  }

  return prefs.getString(key.c_str(), "");
}

int findCard(const String& uid) {
  if (uid.length() == 0) {
    return -1;
  }

  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    if (cardField("cuid", i) == uid) {
      return i;
    }
  }

  return -1;
}

String cardOwner(const String& uid) {
  int slot = findCard(uid);

  if (slot < 0) {
    return "";
  }

  return cardField("cname", (uint8_t)slot);
}

String cardChat(const String& uid) {
  int slot = findCard(uid);

  if (slot < 0) {
    return "";
  }

  return cardField("cchat", (uint8_t)slot);
}

bool saveCard(const String& uid, const String& name, const String& chat) {
  int slot = findCard(uid);

  if (slot < 0) {
    for (uint8_t i = 0; i < MAX_CARDS; i++) {
      if (cardField("cuid", i).length() == 0) {
        slot = i;
        break;
      }
    }
  }

  if (slot < 0) {
    return false;
  }

  String uidKey = String("cuid") + String(slot);
  String nameKey = String("cname") + String(slot);

  String chatKey = String("cchat") + String(slot);

  prefs.putString(uidKey.c_str(), uid);
  prefs.putString(nameKey.c_str(), name);
  prefs.putString(chatKey.c_str(), chat);

  return true;
}

void listCards() {
  Serial.println("----- REGISTERED CARDS -----");

  bool any = false;

  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    String uid = cardField("cuid", i);

    if (uid.length() == 0) {
      continue;
    }

    any = true;
    String chat = cardField("cchat", i);

    if (chat.length() > 0) {
      Serial.println(uid + " -> " + cardField("cname", i) +
                     "  (Telegram chat " + chat + ")");
    } else {
      Serial.println(uid + " -> " + cardField("cname", i) +
                     "  (admin chat)");
    }
  }

  if (!any) {
    Serial.println("(none)");
  }

  Serial.println("----------------------------");
}

void clearCards() {
  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    String uidKey = String("cuid") + String(i);
    String nameKey = String("cname") + String(i);

    if (prefs.isKey(uidKey.c_str())) {
      prefs.remove(uidKey.c_str());
    }

    if (prefs.isKey(nameKey.c_str())) {
      prefs.remove(nameKey.c_str());
    }

    String chatKey = String("cchat") + String(i);

    if (prefs.isKey(chatKey.c_str())) {
      prefs.remove(chatKey.c_str());
    }
  }
}

// Reads one line from the Serial Monitor.
// Returns an empty string on timeout.
String readSerialLine(uint32_t timeoutMs) {
  String line;
  const uint32_t start = millis();

  while ((uint32_t)(millis() - start) < timeoutMs) {
    while (Serial.available()) {
      char ch = (char)Serial.read();

      if (ch == '\n' || ch == '\r') {
        if (line.length() > 0) {
          line.trim();
          return line;
        }
        continue;  // ignore leading line breaks
      }

      if (line.length() < 24) {
        line += ch;
      }
    }

    delay(5);
  }

  return "";
}

// Telegram chat IDs are whole numbers (negative for groups).
bool isValidChatId(const String& s) {
  if (s.length() < 5) {
    return false;
  }

  for (unsigned int i = 0; i < s.length(); i++) {
    char ch = s[i];

    if (i == 0 && ch == '-') {
      continue;
    }

    if (ch < '0' || ch > '9') {
      return false;
    }
  }

  return true;
}

// Serial command 'r': tap a card, type the owner name,
// then type the owner's Telegram chat ID.
void registerCardFlow() {
  if (!rfidOk) {
    Serial.println("[RFID] Reader not detected; cannot register cards.");
    return;
  }

  Serial.println("[RFID] Tap the card to register (20 s)...");

  String uid;
  bool got = false;
  const uint32_t start = millis();

  while ((uint32_t)(millis() - start) < 20000UL) {
    if (readCardUid(uid)) {
      got = true;
      break;
    }
    delay(50);
  }

  if (!got) {
    Serial.println("[RFID] No card tapped. Cancelled.");
    return;
  }

  Serial.println("[RFID] Card UID: " + uid);

  String existing = cardOwner(uid);

  if (existing.length() > 0) {
    Serial.println("[RFID] Already registered to: " + existing +
                   " (it will be renamed)");
  }

  while (Serial.available()) {
    Serial.read();  // clear leftover characters
  }

  Serial.println("[RFID] Type the owner name and press Enter (30 s)...");

  String name = readSerialLine(30000UL);

  if (name.length() == 0) {
    Serial.println("[RFID] No name entered. Cancelled.");
    return;
  }

  String chat = "";
  bool chatOk = false;

  for (uint8_t attempt = 0; attempt < 3 && !chatOk; attempt++) {
    Serial.println("[RFID] Type the owner's Telegram chat ID, or - to use "
                   "the admin chat (30 s)...");

    chat = readSerialLine(30000UL);

    if (chat == "-") {
      chat = "";
      chatOk = true;
    } else if (isValidChatId(chat)) {
      chatOk = true;
    } else {
      Serial.println("[RFID] A chat ID is a number, for example 123456789.");
    }
  }

  if (!chatOk) {
    Serial.println("[RFID] No valid chat ID entered. Cancelled.");
    return;
  }

  if (saveCard(uid, name, chat)) {
    Serial.println("[RFID] Saved: " + uid + " -> " + name);

    if (chat.length() > 0) {
      Serial.println("[RFID] Messages for this card go to chat " + chat +
                     " (the owner must have started the bot).");
    } else {
      Serial.println("[RFID] Messages for this card go to the admin chat.");
    }
  } else {
    Serial.println("[RFID] Card list is full (max 10). Use x to clear.");
  }
}

// =====================================================
// 10. VEHICLE ARRIVAL + TICKET
// =====================================================

void issueTicket(const String& owner,
                 const String& uid,
                 const String& cardText,
                 const String& ownerChat) {
  pendingTicket = false;
  prefs.putBool("pend", false);

  sessionOwner = owner;
  sessionCard = uid;

  sessionChat = ownerChat;

  // Registered cards park free; guests and unregistered cards are charged.
  sessionFree = findCard(uid) >= 0;
  prefs.putBool("ownfree", sessionFree);

  prefs.putString("owner", sessionOwner);
  prefs.putString("ownuid", sessionCard);
  prefs.putString("ownchat", sessionChat);

  ticketNo++;
  prefs.putUInt("ticket", ticketNo);

  String message =
    "PARKING TICKET #" + String(ticketNo) +
    "\nName: " + owner +
    "\nCard: " + cardText +
    "\nSlot: " + cfg.slot +
    "\nEntry: " + fmtTime(entryTime) +
    (sessionFree
      ? String("\nFare: No charge (registered card)")
      : "\nFare: INR " + String(cfg.ratePerHour) + " per started hour") +
    (sessionFree
      ? "\nDashboard: http://" + WiFi.localIP().toString() + "/"
      : String(""));

  routeMessage(sessionChat, message);
}

void handleArrival() {
  setOccupied(true);

  entryTime = time(nullptr);

  prefs.putULong64("entry", (uint64_t)entryTime);

  sessionOwner = "";
  sessionCard = "";
  sessionChat = "";
  prefs.putString("owner", "");
  prefs.putString("ownuid", "");
  prefs.putString("ownchat", "");
  sessionFree = false;
  prefs.putBool("ownfree", false);

  Serial.println("[PARKING] Vehicle arrival confirmed.");

  if (!rfidOk) {
    // No reader available: issue the ticket straight away.
    issueTicket("Guest", "", "reader unavailable", "");
    return;
  }

  pendingTicket = true;
  pendingStart = millis();
  prefs.putBool("pend", true);

  Serial.printf("[RFID] Tap your card within %lu s...\n",
                (unsigned long)(CARD_WINDOW_MS / 1000UL));
}

// Waits for a card tap after arrival; issues the ticket.
void updateRfid() {
  if (!pendingTicket) {
    return;
  }

  // A dashboard card scan owns the reader; keep the ticket window open meanwhile.
  if (scanActive) {
    pendingStart = millis();
    return;
  }

  const uint32_t now = millis();

  if (!rfidOk ||
      (uint32_t)(now - pendingStart) >= CARD_WINDOW_MS) {
    Serial.println("[RFID] No card tapped; issuing guest ticket.");
    issueTicket("Guest", "", "no card tapped", "");
    return;
  }

  if ((uint32_t)(now - lastRfidPoll) < RFID_POLL_MS) {
    return;
  }

  lastRfidPoll = now;

  String uid;

  if (!readCardUid(uid)) {
    return;
  }

  String owner = cardOwner(uid);

  if (owner.length() > 0) {
    Serial.println("[RFID] Card " + uid + " belongs to " + owner);
    String ownerChat = cardChat(uid);

    if (ownerChat.length() == 0) {
      ownerChat = cfg.chatId;
    }

    issueTicket(owner, uid, uid, ownerChat);
  } else {
    Serial.println("[RFID] Unregistered card: " + uid);
    issueTicket("Guest", uid, uid + " (unregistered)", "");
  }
}

// =====================================================
// 11. VEHICLE DEPARTURE + BILLING
// =====================================================

void handleDeparture() {
  setOccupied(false);

  time_t exitTime = time(nullptr);

  if (pendingTicket) {
    // The car left before any ticket was issued.
    pendingTicket = false;
    prefs.putBool("pend", false);

    entryTime = 0;
    prefs.putULong64("entry", 0);

    sessionOwner = "";
    sessionCard = "";
    sessionChat = "";
    prefs.putString("owner", "");
    prefs.putString("ownuid", "");
    prefs.putString("ownchat", "");
    sessionFree = false;
    prefs.putBool("ownfree", false);

    Serial.println("[PARKING] Vehicle left before a ticket was issued.");
    return;
  }

  uint64_t seconds = 0;

  if (entryTime > 0 && exitTime >= entryTime) {
    seconds = (uint64_t)(exitTime - entryTime);
  }

  // Charge for each started hour; minimum one hour.
  uint64_t hours = (seconds + 3599ULL) / 3600ULL;

  if (hours < 1) {
    hours = 1;
  }

  uint64_t fee = sessionFree ? 0ULL : hours * (uint64_t)cfg.ratePerHour;

  Serial.println("[PARKING] Vehicle departure confirmed.");

  String ownerName = sessionOwner.length() > 0 ? sessionOwner : String("Guest");
  String cardText = sessionCard.length() > 0 ? sessionCard : String("none");

  String message =
    "PARKING RECEIPT - Ticket #" + String(ticketNo) +
    "\nName: " + ownerName +
    "\nCard: " + cardText +
    "\nSlot: " + cfg.slot +
    "\nEntry: " + fmtTime(entryTime) +
    "\nExit: " + fmtTime(exitTime) +
    "\nDuration: " + fmtDuration(seconds) +
    (sessionFree
      ? String("\nFee: No charge (registered card)")
      : "\nBillable hours: " + String((unsigned long)hours) +
        "\nFee: INR " + String((unsigned long)fee));

  routeMessage(sessionChat, message);

  entryTime = 0;
  prefs.putULong64("entry", 0);

  sessionOwner = "";
  sessionCard = "";
  sessionChat = "";
  prefs.putString("owner", "");
  prefs.putString("ownuid", "");
  prefs.putString("ownchat", "");
  sessionFree = false;
  prefs.putBool("ownfree", false);
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

    // Readings below the sensor minimum are glitches, not vehicles.
    if (isfinite(distance) && distance < MIN_VALID_CM) {
      distance = NAN;
    }

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

        const uint32_t needed =
          parked ? DEPART_CONFIRM_MS : cfg.confirmMs;

        if ((uint32_t)(now - changeStart) >= needed) {

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

    // Dashboard address, printed after every distance line.
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WEB] Dashboard: http://" +
                     WiFi.localIP().toString() + "/");
    } else {
      Serial.println("[WEB] Dashboard: unavailable (WiFi disconnected)");
    }
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
    server.stop();   // the portal needs port 80

    if (configureWiFi(true)) {
      Serial.println("[CONFIG] Settings updated.");
    } else {
      Serial.println("[CONFIG] Portal failed or settings invalid.");
    }

    server.begin();
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

    Serial.println(rfidOk
      ? "RFID reader: OK"
      : "RFID reader: NOT DETECTED");

    Serial.println(guestChat.length() > 0
      ? "Guest chat: SET"
      : "Guest chat: not set (guests use the admin chat)");

    Serial.println("---------------------------------");
  }

  if (command == 'r' || command == 'R') {
    registerCardFlow();
    sensorInitialized = false;
    lastSensorRead = millis();
  }

  if (command == 'l' || command == 'L') {
    listCards();
  }

  if (command == 'g' || command == 'G') {
    Serial.println("[RFID] Type the Telegram chat ID for unregistered cards "
                   "and guests, or - to clear it (30 s)...");

    String id = readSerialLine(30000UL);

    if (id == "-") {
      guestChat = "";
      prefs.putString("guestchat", "");
      Serial.println("[RFID] Guest chat cleared; guests use the admin chat.");
    } else if (isValidChatId(id)) {
      guestChat = id;
      prefs.putString("guestchat", guestChat);
      Serial.println("[RFID] Guest chat saved: " + guestChat);
    } else if (id.length() == 0) {
      Serial.println("[RFID] No chat ID entered. Cancelled.");
    } else {
      Serial.println("[RFID] A chat ID is a number, for example 123456789.");
    }

    sensorInitialized = false;
    lastSensorRead = millis();
  }

  if (command == 'x' || command == 'X') {
    Serial.println("[RFID] Type YES and press Enter to delete all "
                   "registered cards (15 s)...");

    String answer = readSerialLine(15000UL);

    if (answer == "YES") {
      clearCards();
      Serial.println("[RFID] All registered cards deleted.");
    } else {
      Serial.println("[RFID] Cancelled.");
    }

    sensorInitialized = false;
    lastSensorRead = millis();
  }
}

// =====================================================
// 13B. WEB DASHBOARD (served by the ESP32 itself)
// =====================================================
// Open http://parking.local/ or http://<ESP32-IP>/ on the same WiFi.
// The page is public, but every /api call needs the dashboard PIN
// (header X-Pin). Default PIN: DEFAULT_DASH_PIN - change it in Settings.

static const char DASH_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Parking dashboard</title>
<style>
:root{
  --bg:#e8ecef; --panel:#ffffff; --ink:#1c2630; --muted:#5b6874; --line:#cfd6dc;
  --accent:#0a5cff; --accent-ink:#ffffff; --free:#17825a; --occ:#c2362b; --asphalt:#39424b; --paint:#f2f4f6;
}
@media (prefers-color-scheme:dark){
  :root{--bg:#171c21; --panel:#222a31; --ink:#e7ecf0; --muted:#9aa7b2; --line:#36414a;
        --accent:#5b97ff; --accent-ink:#0b1320; --free:#43c28f; --occ:#ee6a5e; --asphalt:#2c343b; --paint:#cfd6dc;}
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 "Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif}
header{display:flex;align-items:baseline;justify-content:space-between;gap:12px;flex-wrap:wrap;padding:20px 20px 8px;max-width:1100px;margin:0 auto}
h1{margin:0;font-size:1.5rem;font-weight:700;letter-spacing:-.01em}
h2{margin:0 0 12px;font-size:1.05rem;font-weight:700}
.net{color:var(--muted);font-size:.9rem}
main{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(min(100%,420px),1fr));padding:12px 20px 40px;max-width:1100px;margin:0 auto}
section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:18px}
.wide{grid-column:1/-1}
.bay{position:relative;height:190px;border-radius:8px;background:var(--asphalt);border:2px dashed var(--paint);display:flex;align-items:center;justify-content:center;overflow:hidden}
.bay .car{position:absolute;width:58%;height:68%;border-radius:22px 22px 12px 12px;background:var(--occ);box-shadow:inset 0 -10px 0 rgba(0,0,0,.18);transform:translateY(130%);transition:transform .5s ease}
.bay .car::before{content:"";position:absolute;left:14%;right:14%;top:14%;height:30%;border-radius:10px;background:rgba(255,255,255,.55)}
.bay.occupied .car{transform:translateY(0)}
.bay .tag{position:relative;z-index:1;color:var(--paint);font-weight:700;font-size:1.6rem;text-shadow:0 1px 3px rgba(0,0,0,.5)}
.bay.free .tag{color:#fff}
.facts{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:14px 0 0}
.facts dt{color:var(--muted)} .facts dd{margin:0;font-weight:600;word-break:break-word}
.pill{display:inline-block;padding:2px 10px;border-radius:999px;font-size:.85rem;font-weight:600;background:var(--line);color:var(--ink)}
.pill.ok{background:var(--free);color:#fff} .pill.bad{background:var(--occ);color:#fff}
.banner{margin:12px 0 0;padding:10px 12px;border-radius:8px;background:rgba(10,92,255,.12);border:1px solid var(--accent);font-weight:600}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:8px 6px;border-bottom:1px solid var(--line);vertical-align:middle}
th{color:var(--muted);font-weight:600;font-size:.9rem}
td.uid{font-family:ui-monospace,Consolas,monospace;font-size:.92rem}
.empty{color:var(--muted);padding:10px 0}
form{display:grid;gap:12px}
.row{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(150px,1fr))}
label{display:grid;gap:4px;font-size:.9rem;color:var(--muted)}
input{font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:7px;padding:9px 10px;min-width:0}
input:focus-visible,button:focus-visible{outline:3px solid var(--accent);outline-offset:1px}
.hint{font-size:.85rem;color:var(--muted);margin:-4px 0 0}
.actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
button{font:inherit;font-weight:600;border-radius:7px;border:1px solid var(--line);background:var(--panel);color:var(--ink);padding:9px 14px;cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink)}
button.danger{color:var(--occ);border-color:var(--occ)}
button.small{padding:5px 10px;font-size:.88rem}
button:disabled{opacity:.55;cursor:default}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(120px);background:var(--ink);color:var(--bg);padding:10px 16px;border-radius:8px;font-weight:600;transition:transform .25s ease;max-width:90vw;z-index:5}
#toast.show{transform:translateX(-50%) translateY(0)}
#toast.err{background:var(--occ);color:#fff}
dialog{border:1px solid var(--line);border-radius:10px;background:var(--panel);color:var(--ink);padding:20px;max-width:340px;width:90vw}
dialog::backdrop{background:rgba(0,0,0,.45)}
@media (prefers-reduced-motion:reduce){.bay .car,#toast{transition:none}}
</style>
</head>
<body>
<header>
  <h1 id="title">Parking dashboard</h1>
  <div class="net" id="net">Connecting…</div>
</header>

<main>
  <section>
    <h2>Slot status</h2>
    <div class="bay free" id="bay"><div class="car"></div><div class="tag" id="bayTag">Free</div></div>
    <div class="banner" id="pendingBanner" hidden></div>
    <dl class="facts">
      <dt>Distance</dt><dd id="dist">–</dd>
      <dt>Parked by</dt><dd id="owner">–</dd>
      <dt>Card</dt><dd id="card">–</dd>
      <dt>Billing</dt><dd id="bill">–</dd>
      <dt>Entry</dt><dd id="entry">–</dd>
      <dt>Parked for</dt><dd id="elapsed">–</dd>
      <dt>Last ticket</dt><dd id="ticket">–</dd>
      <dt>RFID reader</dt><dd id="rfid">–</dd>
      <dt>Telegram</dt><dd id="tg">–</dd>
    </dl>
    <div class="actions" style="margin-top:14px">
      <button id="btnTest">Send test message</button>
      <button id="btnReboot" class="danger">Restart device</button>
    </div>
  </section>

  <section>
    <h2>Cards on the device <span class="pill" id="cardCount"></span></h2>
    <div id="cardList"></div>
    <hr style="border:none;border-top:1px solid var(--line);margin:16px 0">
    <h2>Save a card</h2>
    <form id="cardForm" autocomplete="off">
      <div class="actions">
        <button type="button" id="btnScan">Scan card on reader</button>
        <span class="hint" id="scanMsg" style="margin:0"></span>
      </div>
      <div class="row">
        <label>Card ID<input id="cUid" placeholder="e.g. 04A1B2C3" required></label>
        <label>Owner name<input id="cName" maxlength="31" placeholder="e.g. Asha" required></label>
      </div>
      <label>Owner's Telegram chat ID
        <input id="cChat" inputmode="numeric" placeholder="Leave empty to use the admin chat">
      </label>
      <p class="hint">The owner must have pressed Start in your bot before messages can reach them. Registered cards park free of charge. Saving a card ID that already exists renames it.</p>
      <div class="actions"><button class="primary" type="submit">Save card</button></div>
    </form>
  </section>

  <section class="wide">
    <h2>Settings</h2>
    <form id="cfgForm" autocomplete="off">
      <div class="row">
        <label>Parking slot<input id="fSlot" maxlength="15" required></label>
        <label>User name<input id="fUser" maxlength="39" required></label>
        <label>Fare per started hour (INR)<input id="fRate" type="number" min="0" max="1000000" required></label>
      </div>
      <div class="row">
        <label>Arrival threshold (cm)<input id="fCar" type="number" step="0.1" min="2" max="350" required></label>
        <label>Departure threshold (cm)<input id="fClear" type="number" step="0.1" min="3" max="400" required></label>
        <label>Arrival confirmation (ms)<input id="fConfirm" type="number" min="500" max="60000" required></label>
      </div>
      <div class="row">
        <label>Admin Telegram chat ID<input id="fChat" inputmode="numeric"></label>
        <label>Guest chat ID<input id="fGuest" inputmode="numeric" placeholder="Empty = use admin chat"></label>
        <label>Bot token<input id="fToken" type="password" placeholder="Leave empty to keep the saved token"></label>
        <label>New dashboard PIN<input id="fPin" type="password" minlength="4" maxlength="12" placeholder="Leave empty to keep"></label>
      </div>
      <div class="actions"><button class="primary" type="submit">Save settings</button></div>
    </form>
  </section>
</main>

<div id="toast" role="status" aria-live="polite"></div>

<dialog id="pinDlg">
  <form method="dialog" id="pinForm">
    <h2>Dashboard PIN</h2>
    <label>Enter the PIN<input id="pinInput" type="password" autocomplete="current-password" required></label>
    <div class="actions" style="margin-top:12px"><button class="primary" type="submit">Unlock</button></div>
  </form>
</dialog>

<script>
const $ = s => document.querySelector(s);
let pin = sessionStorage.getItem('pin') || '';
let pinWait = null;
let clockOffset = 0;

function toast(msg, isErr) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'show' + (isErr ? ' err' : '');
  clearTimeout(toast.h);
  toast.h = setTimeout(() => { t.className = ''; }, 3200);
}

function askPin() {
  if (pinWait) return pinWait;
  pinWait = new Promise(resolve => {
    const dlg = $('#pinDlg');
    $('#pinInput').value = '';
    dlg.showModal();
    $('#pinForm').onsubmit = () => {
      pin = $('#pinInput').value;
      sessionStorage.setItem('pin', pin);
      pinWait = null;
      resolve();
    };
  });
  return pinWait;
}

async function api(path, opt) {
  opt = Object.assign({}, opt);
  for (let attempt = 0; attempt < 3; attempt++) {
    const headers = Object.assign({}, opt.headers, { 'X-Pin': pin });
    let body = opt.body;
    if (body && typeof body !== 'string') {
      body = JSON.stringify(body);
      headers['Content-Type'] = 'application/json';
    }
    const r = await fetch(path, { method: opt.method || 'GET', headers, body });
    if (r.status === 401) { await askPin(); continue; }
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.error || ('Request failed (' + r.status + ')'));
    return j;
  }
  throw new Error('PIN rejected');
}

function fmtDur(s) {
  s = Math.max(0, Math.floor(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return (h ? h + ' h ' : '') + (h || m ? m + ' min ' : '') + sec + ' s';
}

let last = null;
function renderStatus(d) {
  last = d;
  clockOffset = d.now - Date.now() / 1000;
  document.title = 'Slot ' + d.slot + ' · Parking';
  $('#title').textContent = 'Slot ' + d.slot + ' · ' + d.user;
  $('#net').textContent = d.ssid + ' · ' + d.ip + ' · ' + d.rssi + ' dBm';
  $('#bay').className = 'bay ' + (d.parked ? 'occupied' : 'free');
  $('#bayTag').textContent = d.parked ? 'Occupied' : 'Free';
  $('#dist').textContent = d.distance === null ? 'No echo' : d.distance.toFixed(1) + ' cm';
  $('#owner').textContent = d.parked ? (d.pending ? 'Waiting for card tap' : (d.owner || 'Guest')) : '–';
  $('#card').textContent = d.parked && d.card ? d.card : '–';
  $('#bill').textContent = d.parked && !d.pending ? (d.free ? 'No charge (registered card)' : 'INR ' + d.rate + ' per started hour') : '–';
  $('#entry').textContent = d.parked && d.entryText ? d.entryText : '–';
  $('#ticket').textContent = '#' + d.ticket;
  $('#rfid').innerHTML = d.rfidOk ? '<span class="pill ok">Detected</span>' : '<span class="pill bad">Not detected</span>';
  $('#tg').innerHTML = (d.botSet && d.chatSet) ? '<span class="pill ok">Configured</span>' : '<span class="pill bad">Missing token or chat ID</span>';
  const b = $('#pendingBanner');
  b.hidden = !d.pending;
  if (d.pending) b.textContent = 'Car detected. Tap a card on the reader within ' + d.pendingLeft + ' s to issue the ticket.';
  tickElapsed();
}
function tickElapsed() {
  if (!last || !last.parked || !last.entry || !last.timeOk) { $('#elapsed').textContent = '–'; return; }
  $('#elapsed').textContent = fmtDur(Date.now() / 1000 + clockOffset - last.entry);
}
setInterval(tickElapsed, 1000);

async function pollStatus() {
  try {
    if (!pinWait) renderStatus(await api('/api/status'));
  } catch (e) {
    $('#net').textContent = 'Device not reachable';
  }
  setTimeout(pollStatus, 1500);
}

async function loadCards() {
  try {
    const d = await api('/api/cards');
    $('#cardCount').textContent = d.cards.length + ' of ' + d.max;
    const box = $('#cardList');
    if (!d.cards.length) { box.innerHTML = '<div class="empty">No cards saved yet. Tickets go to the guest chat until you add one.</div>'; return; }
    const rows = d.cards.map(c =>
      '<tr><td class="uid"></td><td class="nm"></td><td class="ch"></td><td><button class="small danger" data-uid="">Delete</button></td></tr>');
    box.innerHTML = '<table><thead><tr><th>Card ID</th><th>Owner</th><th>Chat</th><th></th></tr></thead><tbody>' + rows.join('') + '</tbody></table>';
    box.querySelectorAll('tbody tr').forEach((tr, i) => {
      const c = d.cards[i];
      tr.querySelector('.uid').textContent = c.uid;
      tr.querySelector('.nm').textContent = c.name;
      tr.querySelector('.ch').textContent = c.chat || 'Admin chat';
      const btn = tr.querySelector('button');
      btn.dataset.uid = c.uid;
      btn.onclick = () => delCard(c);
    });
  } catch (e) { toast(e.message, true); }
}

async function delCard(c) {
  if (!confirm('Delete the card for ' + c.name + ' (' + c.uid + ')?')) return;
  try { await api('/api/cards?uid=' + encodeURIComponent(c.uid), { method: 'DELETE' }); toast('Card deleted'); loadCards(); }
  catch (e) { toast(e.message, true); }
}

$('#cardForm').onsubmit = async ev => {
  ev.preventDefault();
  try {
    await api('/api/cards', { method: 'POST', body: { uid: $('#cUid').value, name: $('#cName').value, chat: $('#cChat').value } });
    toast('Card saved to the device');
    ev.target.reset(); $('#scanMsg').textContent = '';
    loadCards();
  } catch (e) { toast(e.message, true); }
};

let scanTimer = null;
$('#btnScan').onclick = async () => {
  try {
    await api('/api/scan/start', { method: 'POST', body: {} });
    $('#btnScan').disabled = true;
    $('#scanMsg').textContent = 'Hold the card on the reader…';
    const poll = async () => {
      try {
        const s = await api('/api/scan');
        if (s.uid) {
          $('#cUid').value = s.uid;
          if (s.known) { $('#cName').value = s.name; $('#cChat').value = s.chat; }
          $('#scanMsg').textContent = s.known ? 'Card already saved. Saving will update it.' : 'Card read. Add the owner name and save.';
          $('#btnScan').disabled = false; $('#cName').focus(); return;
        }
        if (!s.active) { $('#scanMsg').textContent = 'No card read. Try again.'; $('#btnScan').disabled = false; return; }
        $('#scanMsg').textContent = 'Hold the card on the reader… ' + s.left + ' s';
        scanTimer = setTimeout(poll, 700);
      } catch (e) { $('#btnScan').disabled = false; toast(e.message, true); }
    };
    poll();
  } catch (e) { toast(e.message, true); }
};

async function loadConfig() {
  try {
    const c = await api('/api/config');
    $('#fSlot').value = c.slot; $('#fUser').value = c.user; $('#fRate').value = c.rate;
    $('#fCar').value = c.carCm; $('#fClear').value = c.clearCm; $('#fConfirm').value = c.confirmMs;
    $('#fChat').value = c.chatId; $('#fGuest').value = c.guestChat;
  } catch (e) { toast(e.message, true); }
}

$('#cfgForm').onsubmit = async ev => {
  ev.preventDefault();
  const body = {
    slot: $('#fSlot').value, user: $('#fUser').value, rate: +$('#fRate').value,
    carCm: +$('#fCar').value, clearCm: +$('#fClear').value, confirmMs: +$('#fConfirm').value,
    chatId: $('#fChat').value, guestChat: $('#fGuest').value,
    botToken: $('#fToken').value, dashPin: $('#fPin').value
  };
  try {
    await api('/api/config', { method: 'POST', body });
    if (body.dashPin) { pin = body.dashPin; sessionStorage.setItem('pin', pin); }
    $('#fToken').value = ''; $('#fPin').value = '';
    toast('Settings saved');
  } catch (e) { toast(e.message, true); }
};

$('#btnTest').onclick = async () => {
  $('#btnTest').disabled = true;
  try { await api('/api/test', { method: 'POST', body: {} }); toast('Test message sent'); }
  catch (e) { toast(e.message, true); }
  $('#btnTest').disabled = false;
};

$('#btnReboot').onclick = async () => {
  if (!confirm('Restart the ESP32? The dashboard will reconnect in a few seconds.')) return;
  try { await api('/api/reboot', { method: 'POST', body: {} }); toast('Restarting…'); } catch (e) { toast(e.message, true); }
};

(async () => {
  try { await api('/api/status'); } catch (e) {}
  loadCards(); loadConfig(); pollStatus();
})();
</script>
</body>
</html>
)rawliteral";

static void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(code, d);
}

static bool authed() {
  if (dashPin.length() > 0 && server.header("X-Pin") != dashPin) {
    sendError(401, "PIN required or wrong");
    return false;
  }
  return true;
}

static bool readBody(JsonDocument& doc) {
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendError(400, "Invalid JSON");
    return false;
  }
  return true;
}

// MFRC522 UIDs are 4, 7 or 10 bytes -> 8, 14 or 20 hex characters.
static bool isValidUid(const String& s) {
  if (s.length() < 8 || s.length() > 20 || (s.length() % 2) != 0) {
    return false;
  }
  for (unsigned int i = 0; i < s.length(); i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return false;
    }
  }
  return true;
}

static bool deleteCard(const String& uid) {
  int slot = findCard(uid);

  if (slot < 0) {
    return false;
  }

  const char* prefixes[] = {"cuid", "cname", "cchat"};

  for (const char* p : prefixes) {
    String key = String(p) + String(slot);

    if (prefs.isKey(key.c_str())) {
      prefs.remove(key.c_str());
    }
  }

  return true;
}

// ---------- API handlers ----------

static void apiStatus() {
  if (!authed()) return;

  JsonDocument d;

  d["slot"] = cfg.slot;
  d["user"] = cfg.userName;
  d["parked"] = parked;
  d["pending"] = pendingTicket;

  if (pendingTicket) {
    uint32_t elapsed = millis() - pendingStart;
    d["pendingLeft"] = elapsed >= CARD_WINDOW_MS
      ? 0 : (unsigned long)((CARD_WINDOW_MS - elapsed) / 1000UL);
  }

  if (lastDistanceValid) {
    d["distance"] = roundf(lastDistanceCm * 10.0f) / 10.0f;
  } else {
    d["distance"] = nullptr;
  }

  d["owner"] = sessionOwner;
  d["card"] = sessionCard;
  d["ticket"] = ticketNo;
  d["free"] = sessionFree;
  d["rate"] = cfg.ratePerHour;
  d["entry"] = (uint32_t)entryTime;
  d["entryText"] = entryTime > 0 ? fmtTime(entryTime) : String("");
  d["now"] = (uint32_t)time(nullptr);
  d["timeOk"] = timeIsValid();
  d["rfidOk"] = rfidOk;
  d["botSet"] = cfg.botToken.length() > 0 &&
                cfg.botToken != "PASTE_YOUR_NEW_BOT_TOKEN_HERE";
  d["chatSet"] = cfg.chatId.length() > 0;
  d["ssid"] = WiFi.SSID();
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();

  sendJson(200, d);
}

static void apiCardsGet() {
  if (!authed()) return;

  JsonDocument d;
  JsonArray arr = d["cards"].to<JsonArray>();

  for (uint8_t i = 0; i < MAX_CARDS; i++) {
    String uid = cardField("cuid", i);

    if (uid.length() == 0) {
      continue;
    }

    JsonObject o = arr.add<JsonObject>();
    o["uid"] = uid;
    o["name"] = cardField("cname", i);
    o["chat"] = cardField("cchat", i);
  }

  d["max"] = MAX_CARDS;
  sendJson(200, d);
}

static void apiCardSave() {
  if (!authed()) return;

  JsonDocument doc;
  if (!readBody(doc)) return;

  String uid = doc["uid"] | "";
  String name = doc["name"] | "";
  String chat = doc["chat"] | "";

  uid.replace(":", "");
  uid.replace(" ", "");
  uid.trim();
  uid.toUpperCase();
  name.trim();
  chat.trim();

  if (!isValidUid(uid)) {
    sendError(400, "Card ID must be 8, 14 or 20 hex characters (e.g. 04A1B2C3)");
    return;
  }

  if (name.length() == 0 || name.length() > 31) {
    sendError(400, "Owner name must be 1 to 31 characters");
    return;
  }

  if (chat.length() > 0 && !isValidChatId(chat)) {
    sendError(400, "Chat ID must be a number, e.g. 123456789");
    return;
  }

  if (!saveCard(uid, name, chat)) {
    sendError(409, "Card list is full. Delete a card first.");
    return;
  }

  Serial.println("[WEB] Card saved: " + uid + " -> " + name);

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

static void apiCardDelete() {
  if (!authed()) return;

  String uid = server.arg("uid");
  uid.toUpperCase();

  if (!deleteCard(uid)) {
    sendError(404, "Card not found");
    return;
  }

  Serial.println("[WEB] Card deleted: " + uid);

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

static void apiScanStart() {
  if (!authed()) return;

  if (!rfidOk) {
    sendError(400, "RFID reader not detected. Check wiring and restart.");
    return;
  }

  scanActive = true;
  scanStart = millis();
  scanUid = "";

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

static void apiScanGet() {
  if (!authed()) return;

  JsonDocument d;
  d["active"] = scanActive;
  d["uid"] = scanUid;

  if (scanActive) {
    uint32_t elapsed = millis() - scanStart;
    d["left"] = elapsed >= SCAN_WINDOW_MS
      ? 0 : (unsigned long)((SCAN_WINDOW_MS - elapsed) / 1000UL);
  }

  if (scanUid.length() > 0) {
    d["known"] = findCard(scanUid) >= 0;
    d["name"] = cardOwner(scanUid);
    d["chat"] = cardChat(scanUid);
  }

  sendJson(200, d);
}

static void apiConfigGet() {
  if (!authed()) return;

  JsonDocument d;
  d["slot"] = cfg.slot;
  d["user"] = cfg.userName;
  d["carCm"] = cfg.carCm;
  d["clearCm"] = cfg.clearCm;
  d["confirmMs"] = cfg.confirmMs;
  d["rate"] = cfg.ratePerHour;
  d["chatId"] = cfg.chatId;
  d["guestChat"] = guestChat;
  sendJson(200, d);
}

static void apiConfigSave() {
  if (!authed()) return;

  JsonDocument doc;
  if (!readBody(doc)) return;

  String slot = doc["slot"] | cfg.slot.c_str();
  String user = doc["user"] | cfg.userName.c_str();
  String chat = doc["chatId"] | cfg.chatId.c_str();
  String guest = doc["guestChat"] | guestChat.c_str();
  String token = doc["botToken"] | "";
  String newPin = doc["dashPin"] | "";

  float car = doc["carCm"] | cfg.carCm;
  float clear = doc["clearCm"] | cfg.clearCm;
  uint32_t confirm = doc["confirmMs"] | cfg.confirmMs;
  uint32_t rate = doc["rate"] | cfg.ratePerHour;

  slot.trim();
  user.trim();
  chat.trim();
  guest.trim();
  token.trim();
  newPin.trim();

  if (slot.length() == 0 || slot.length() > 15 ||
      user.length() == 0 || user.length() > 39) {
    sendError(400, "Slot (max 15) and user name (max 39) are required");
    return;
  }

  if (!isfinite(car) || !isfinite(clear) ||
      car < 2.0f || car > 350.0f ||
      clear <= car || clear > 400.0f) {
    sendError(400, "Thresholds: arrival 2-350 cm, departure must be larger (max 400)");
    return;
  }

  if (confirm < 500 || confirm > 60000 || rate > 1000000UL) {
    sendError(400, "Confirmation must be 500-60000 ms and fare at most 1000000");
    return;
  }

  if (chat.length() > 0 && !isValidChatId(chat)) {
    sendError(400, "Admin chat ID must be a number");
    return;
  }

  if (guest.length() > 0 && !isValidChatId(guest)) {
    sendError(400, "Guest chat ID must be a number");
    return;
  }

  if (token.length() > 0 && (token.length() < 30 || token.indexOf(':') < 0)) {
    sendError(400, "That does not look like a Telegram bot token");
    return;
  }

  if (newPin.length() > 0 && (newPin.length() < 4 || newPin.length() > 12)) {
    sendError(400, "PIN must be 4 to 12 characters");
    return;
  }

  cfg.slot = slot;
  cfg.userName = user;
  cfg.chatId = chat;
  cfg.carCm = car;
  cfg.clearCm = clear;
  cfg.confirmMs = confirm;
  cfg.ratePerHour = rate;

  if (token.length() > 0) {
    cfg.botToken = token;
  }

  saveConfig();

  guestChat = guest;
  prefs.putString("guestchat", guestChat);

  if (newPin.length() > 0) {
    dashPin = newPin;
    prefs.putString("dashpin", dashPin);
  }

  sensorInitialized = false;   // re-evaluate with the new thresholds
  lastSensorRead = millis();

  Serial.println("[WEB] Settings saved.");

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

static void apiTest() {
  if (!authed()) return;

  if (cfg.chatId.length() == 0) {
    sendError(400, "Set the admin Telegram chat ID first");
    return;
  }

  bool ok = sendTelegram("Dashboard test message. Slot " + cfg.slot +
                         " is " + String(parked ? "occupied." : "free."));

  if (!ok) {
    sendError(502, "Telegram did not accept the message. Check token and chat ID.");
    return;
  }

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

static void apiReboot() {
  if (!authed()) return;

  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);

  delay(300);
  ESP.restart();
}

// Called from loop(): waits for a card while the dashboard scan is active.
void updateScan() {
  if (!scanActive) {
    return;
  }

  const uint32_t now = millis();

  if ((uint32_t)(now - scanStart) >= SCAN_WINDOW_MS) {
    scanActive = false;
    return;
  }

  if ((uint32_t)(now - lastRfidPoll) < RFID_POLL_MS) {
    return;
  }

  lastRfidPoll = now;

  String uid;

  if (readCardUid(uid)) {
    scanUid = uid;
    scanActive = false;
    Serial.println("[WEB] Scanned card: " + uid);
  }
}

void startWebServer() {
  dashPin = prefs.isKey("dashpin")
    ? prefs.getString("dashpin", DEFAULT_DASH_PIN)
    : String(DEFAULT_DASH_PIN);

  if (dashPin.length() == 0) {
    dashPin = DEFAULT_DASH_PIN;
  }

  if (MDNS.begin("parking")) {
    MDNS.addService("http", "tcp", 80);
  }

  static const char* headerKeys[] = {"X-Pin"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", DASH_HTML);
  });

  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/cards", HTTP_GET, apiCardsGet);
  server.on("/api/cards", HTTP_POST, apiCardSave);
  server.on("/api/cards", HTTP_DELETE, apiCardDelete);
  server.on("/api/scan/start", HTTP_POST, apiScanStart);
  server.on("/api/scan", HTTP_GET, apiScanGet);
  server.on("/api/config", HTTP_GET, apiConfigGet);
  server.on("/api/config", HTTP_POST, apiConfigSave);
  server.on("/api/test", HTTP_POST, apiTest);
  server.on("/api/reboot", HTTP_POST, apiReboot);

  server.onNotFound([]() {
    sendError(404, "Not found");
  });

  server.begin();

  Serial.println("[WEB] Dashboard: http://parking.local/  or  http://" +
                 WiFi.localIP().toString() + "/");
  Serial.println("[WEB] Default PIN is " + String(DEFAULT_DASH_PIN) +
                 " (change it in Settings).");

  // Send the dashboard address to Telegram so it can be opened from a phone.
  if (cfg.chatId.length() > 0 &&
      cfg.botToken.length() > 0 &&
      cfg.botToken != "PASTE_YOUR_NEW_BOT_TOKEN_HERE") {
    sendTelegram("Parking dashboard is online: http://" +
                 WiFi.localIP().toString() + "/");
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
  Serial.println("  r = Register an RFID card");
  Serial.println("  l = List registered cards");
  Serial.println("  g = Set the guest chat ID (unregistered cards)");
  Serial.println("  x = Delete all registered cards");

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();

  byte rfidVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidOk = (rfidVersion != 0x00 && rfidVersion != 0xFF);

  Serial.printf("[RFID] RC522 firmware version: 0x%02X (%s)\n",
                rfidVersion,
                rfidOk ? "OK" : "NOT DETECTED - tickets will be guest only");

  if (!configureWiFi(false)) {
    Serial.println("[SYSTEM] WiFi setup failed; restarting.");
    delay(5000);
    ESP.restart();
  }

  initializeTime();
  startWebServer();

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

  server.handleClient();
  updateParkingDetection();
  updateScan();
  updateRfid();

  delay(2);
}
