# Smart Parking Slot Monitor

An ESP32-based parking slot monitor. An ultrasonic sensor detects when a vehicle enters a slot, and the firmware automatically sends a digital parking ticket to a Telegram chat. When the vehicle leaves, it sends an exit receipt with the duration and a fee. A Raspberry Pi Pico 2 W shows the slot status on red and green LEDs.

<!-- Add a photo or short demo GIF of the model slot and the Telegram messages here -->

## Features

- Ultrasonic vehicle detection (HC-SR04) on an ESP32
- 5-second confirmation window to ignore people or objects passing by
- Hysteresis (separate enter and leave thresholds) to stop flicker near the limit
- Tickets with ticket number, slot ID, and NTP-synced timestamp, sent over HTTPS via the Telegram Bot API
- Exit receipts with entry time, exit time, duration (seconds, minutes, hours), and a fee per started hour
- Ticket counter stored in flash (ESP32 NVS, `Preferences` library), so numbering survives restarts
- Pico 2 W slot indicator: green = free, red = occupied, driven by one GPIO line from the ESP32

## Example messages

```
PARKING TICKET #1006
Name: Your Name
Slot: A1
Entry: 17 Oct 14:02:11
```

```
EXIT - Ticket #1006
Name: Your Name
Slot: A1
Entry: 17 Oct 14:02:11
Exit: 17 Oct 14:05:26
Duration: 3 min 15 s
Fee: 20
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
| Cardboard slot and toy car | | Testing |

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

Both boards use 3.3 V logic, so no level shifting is needed. Connect only these wires between the two boards.

## Software setup

1. Install the Arduino IDE.
2. Add board support:
   - ESP32: the Espressif **esp32** board package
   - Pico 2 W: **Raspberry Pi Pico/RP2040/RP2350** by Earle F. Philhower, III. Add this URL under *Additional boards manager URLs*:
     `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
3. Create a Telegram bot with [@BotFather](https://t.me/BotFather) and note the bot token. Get your chat ID by messaging the bot and opening `https://api.telegram.org/bot<TOKEN>/getUpdates`.
4. Open `esp32_parking/esp32_parking.ino` and fill in the placeholders at the top:

   ```cpp
   const char* ssid      = "YOUR_WIFI_NAME";   // 2.4 GHz network
   const char* pass      = "YOUR_WIFI_PASSWORD";
   const char* BOT_TOKEN = "YOUR_BOT_TOKEN";
   const char* CHAT_ID   = "YOUR_CHAT_ID";
   const char* SLOT      = "A1";
   const char* USER_NAME = "Your Name";
   ```

5. Upload it to the ESP32.
6. Open `pico_indicator/pico_indicator.ino`, select **Raspberry Pi Pico 2 W**, and upload it to the Pico.

> **Never commit your real WiFi password, bot token, or chat ID.** If a token leaks, revoke it in BotFather with `/revoke`.

### Uploading to the Pico on Linux with the Flatpak Arduino IDE

The Flatpak sandbox has no `udisksctl`, so the IDE can compile but not copy the file. Instead, compile with *Verify*, hold **BOOTSEL** while plugging in the Pico, and copy the `.uf2` file onto the `RP2350` drive.

## How it works

1. The ESP32 connects to WiFi and syncs its clock over NTP.
2. Every 200 ms it measures the distance to the nearest object.
3. If the distance stays below `CAR_CM` (10 cm) for 5 seconds, a car is parked. The ESP32 sets GPIO23 HIGH, increments and saves the ticket number, and sends the ticket to Telegram.
4. Once parked, the car only counts as gone when the distance rises above `CLEAR_CM` (15 cm) for 5 seconds. The gap between the two thresholds is the hysteresis.
5. On exit, the ESP32 sets GPIO23 LOW and sends the receipt. The fee is `RATE_PER_HOUR` for each started hour, with a minimum of one hour.
6. The Pico reads GPIO15 and switches between the green and red LEDs.

## Configuration

| Constant | Default | Meaning |
|---|---|---|
| `CAR_CM` | 10 | Closer than this means a car arrives (cm) |
| `CLEAR_CM` | 15 | Farther than this means the car left (cm) |
| `CONFIRM_MS` | 5000 | Time a change must hold before it counts |
| `RATE_PER_HOUR` | 20 | Fee per started hour |
| `GMT_OFFSET_SEC` | 19800 | Time zone offset (UTC+5:30) |

Raise `CAR_CM` and `CLEAR_CM` (keeping a gap of a few cm) for a real slot.

## Repository layout

```
esp32_parking/esp32_parking.ino     ESP32 firmware (sensor, Telegram, NVS)
pico_indicator/pico_indicator.ino   Pico 2 W LED indicator
README.md
```

## Known limitations

- It detects any solid object, not specifically cars.
- Messages go to one fixed Telegram chat, so it doesn't know which driver owns the slot, and the name on tickets is a fixed setting.
- Only the ticket counter is saved. An active ticket's entry time is lost if the ESP32 restarts while a car is parked.
- The HTTPS connection uses `setInsecure()`, which skips certificate validation. This is fine for a demo, but a real deployment should pin the Telegram certificate.
- The HC-SR04 is not weatherproof, and at 3.3 V some units give unstable readings.

## Possible next steps

- Save the entry time in flash so active tickets survive a restart
- Add more slot nodes (the Pico 2 W can run a second sensor over WiFi)
- Add an RFID reader (RC522) to link a driver to each ticket
- A `/status` bot command showing free slots
- A small server for ticket storage
- A waterproof sensor (JSN-SR04T) for outdoor use
