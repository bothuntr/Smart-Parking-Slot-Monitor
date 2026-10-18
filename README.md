# Smart Parking Slot Monitor

An ESP32-based parking slot monitor. An ultrasonic sensor detects when a vehicle enters a slot, and the firmware sends a digital parking ticket to a Telegram chat. When the vehicle leaves, it sends a receipt with the duration and the fare. A Raspberry Pi Pico 2 W shows the slot status on red and green LEDs.

## Features

- Ultrasonic vehicle detection (HC-SR04) on an ESP32
- Configurable confirmation window (default 5 s) to ignore people or objects passing by
- Hysteresis (separate arrival and departure thresholds) to stop flicker near the limit
- Tickets and receipts sent over HTTPS through the Telegram Bot API, with NTP-synced timestamps
- Fare per started hour, with a minimum of one hour (default INR 20 per hour)
- Duration shown in seconds, minutes, or hours
- **Parking state survives restarts**: the ticket counter, the occupied flag, and the entry time are saved in flash (NVS) and restored on boot
- **Setup portal (WiFiManager)**: WiFi, slot name, user name, Telegram credentials, thresholds, confirmation time, and fare are set from a phone, with no re-flashing
- Serial commands for configuration and diagnostics, and a distance log every 2 seconds
- Pico 2 W slot indicator: green = free, red = occupied, driven by one GPIO line from the ESP32

## Example messages

```
PARKING TICKET #1006
Name: Your Name
Slot: A1
Entry: 17 Oct 2026 14:02:11
Fare: INR 20 per started hour
```

```
PARKING RECEIPT - Ticket #1006
Name: Your Name
Slot: A1
Entry: 17 Oct 2026 14:02:11
Exit: 17 Oct 2026 14:05:26
Duration: 3 min 15 s
Billable hours: 1
Fee: INR 20
```

## Hardware

| Part | Qty | Purpose |
|---|---|---|
| ESP32 dev board | 1 | Sensing, WiFi, Telegram |
| HC-SR04 ultrasonic sensor | 1 | Distance measurement |
| Raspberry Pi Pico 2 W | 1 | LED status indicator |
| Red LED and green LED | 1 each | Occupied and free indicators |
| 220 Ω resistors | 2 | LED current limiting |
| Breadboard and jumper wires | | Wiring |
| 3D-printed housing (optional) | 1 | Holds the sensor |

## Wiring

**HC-SR04 to ESP32**

| HC-SR04 | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| TRIG | GPIO5 |
| ECHO | GPIO18 |

**ESP32 to Pico 2 W**

| ESP32 | Pico 2 W |
|---|---|
| GPIO23 | GP15 (input, HIGH = occupied) |
| GND | GND (shared ground is required) |

**LEDs on the Pico 2 W**

| Pico pin | Connection |
|---|---|
| GP13 | 220 Ω resistor, then green LED anode. Cathode to GND |
| GP14 | 220 Ω resistor, then red LED anode. Cathode to GND |

Both boards use 3.3 V logic, so no level shifting is needed.

## Software setup

1. Install the Arduino IDE.
2. Add board support:
   - ESP32: the Espressif **esp32** board package
   - Pico 2 W: **Raspberry Pi Pico/RP2040/RP2350** by Earle F. Philhower, III. Add this URL under *Additional boards manager URLs*:
     `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
3. Install these libraries from the Library Manager:
   - **WiFiManager** by tzapu
   - **ArduinoJson** (version 7 or newer)
4. Create a Telegram bot with [@BotFather](https://t.me/BotFather) and note the bot token. Get your chat ID by messaging the bot and opening `https://api.telegram.org/bot<TOKEN>/getUpdates`.
5. In `esp32_parking/esp32_parking.ino`, set the setup portal password:

   ```cpp
   const char* AP_PASSWORD = "change-this-password";   // min. 8 characters
   ```

   Leave `BOT_TOKEN` and `CHAT_ID` as placeholders. You enter the real values in the portal (next section), so they never go into Git.
6. Upload the sketch to the ESP32.
7. Open `pico_indicator/pico_indicator.ino`, select **Raspberry Pi Pico 2 W**, and upload it to the Pico.

### First boot and configuration

1. On first boot (or when no WiFi is saved) the ESP32 opens a WiFi access point named **Parking-Setup**. Connect to it with your phone, using the `AP_PASSWORD` you set.
2. The setup page opens. Choose your 2.4 GHz WiFi network, enter its password, and fill in the parking settings: slot, user name, Telegram bot token, Telegram chat ID, thresholds, confirmation time, and fare.
3. Save. The ESP32 connects, stores the settings in flash, and starts monitoring.
4. To change settings later, open the Serial Monitor at **115200 baud** and send `c` to reopen the portal, or `s` to print the current settings.

> **Never commit your real WiFi password, bot token, or chat ID.** If a token leaks, revoke it in BotFather with `/revoke`.

### Uploading to the Pico on Linux with the Flatpak Arduino IDE

The Flatpak sandbox has no `udisksctl`, so the IDE can compile but not copy the file. Instead, compile with *Verify*, hold **BOOTSEL** while plugging in the Pico, and copy the `.uf2` file onto the `RP2350` drive.

## How it works

1. The ESP32 connects to WiFi (through the portal on first run) and syncs its clock over NTP.
2. Every 200 ms it measures the distance to the nearest object. A reading with no echo is ignored, not treated as an empty slot.
3. If the distance stays below the arrival threshold for the confirmation time, a car is parked. The ESP32 sets GPIO23 HIGH, saves the new ticket number, occupied flag, and entry time, and sends the ticket.
4. Once parked, the car only counts as gone when the distance rises above the departure threshold for the confirmation time. The gap between the two thresholds is the hysteresis.
5. On departure, the ESP32 sets GPIO23 LOW, calculates the fee, and sends the receipt.
6. The Pico reads GPIO15 and switches between the green and red LEDs.

## Default settings

| Setting | Default | Meaning |
|---|---|---|
| Arrival threshold | 10 cm | Closer than this means a car arrives |
| Departure threshold | 15 cm | Farther than this means the car left |
| Confirmation time | 5000 ms | Time a change must hold before it counts |
| Fare | INR 20 | Per started hour |
| Time zone | UTC+5:30 | `GMT_OFFSET_SEC` in the sketch |

Raise the two thresholds, keeping a gap of a few cm, for a real slot. All of them can be changed in the setup portal except the time zone.

## Known limitations

- It detects any solid object, not specifically cars.
- Messages go to one fixed Telegram chat, so it doesn't know which driver owns the slot, and the name on tickets is a single setting.
- The HTTPS connection uses `setInsecure()`, which skips certificate validation. This is fine for a demo, but a real deployment should pin the Telegram certificate.
- The bot token is stored in the ESP32's flash without encryption.
- The HC-SR04 is not weatherproof, and at 3.3 V some units give unstable readings.

## Possible next steps

- Add more slot nodes (the Pico 2 W can run a second sensor over WiFi)
- Add an RFID reader (RC522) to link a driver to each ticket
- A `/status` bot command showing free slots
- A small server for ticket storage
- A waterproof sensor (JSN-SR04T) for outdoor use
