# Smart Parking Slot Monitor

An ESP32-based parking slot monitor. An ultrasonic sensor detects when a vehicle enters a slot, and the driver taps an RFID card to identify themselves. The firmware then sends a digital parking ticket to a Telegram chat. When the vehicle leaves, it sends a receipt with the owner, the duration and the fare. A Raspberry Pi Pico 2 W shows the slot status on red and green LEDs.

## Features

- Ultrasonic vehicle detection (HC-SR04) on an ESP32
- **RFID driver identification (RC522)**: after a car is detected, the driver has 30 s to tap a registered card, and the ticket shows the owner's name and card ID
- Cards are registered from the Serial Monitor and stored in flash (NVS). Unregistered cards and missed taps get a clearly marked "Guest" ticket
- **Per-driver Telegram delivery**: each card is linked to its own Telegram chat ID, so a driver's ticket and receipt go only to their account. Unregistered cards and missed taps go to a separate guest (test) chat
- Configurable confirmation window (default 5 s) to ignore people or objects passing by
- Faster departure confirmation (2 s), so the light turns green and the receipt goes out quickly after the car leaves
- Readings below 2 cm are rejected as sensor glitches
- Hysteresis (separate arrival and departure thresholds) to stop flicker near the limit
- Tickets and receipts sent over HTTPS through the Telegram Bot API, with NTP-synced timestamps
- Fare per started hour, with a minimum of one hour (default INR 20 per hour)
- Duration shown in seconds, minutes, or hours
- **Parking state survives restarts**: the ticket counter, the occupied flag, the entry time and the session owner are saved in flash (NVS) and restored on boot
- **Setup portal (WiFiManager)**: WiFi, slot name, Telegram credentials, thresholds, confirmation time, and fare are set from a phone, with no re-flashing
- Serial commands for configuration, card management and diagnostics, and a distance log every 2 seconds
- Pico 2 W slot indicator: green = free, red = occupied, driven by one GPIO line from the ESP32

## Example messages

Ticket for a registered card:

```
PARKING TICKET #1006
Name: Rahul
Card: A1B2C3D4
Slot: A1
Entry: 17 Oct 2026 14:02:11
Fare: INR 20 per started hour
```

Ticket when nobody taps a card in time (an unregistered card shows its UID and "(unregistered)" instead):

```
PARKING TICKET #1007
Name: Guest
Card: no card tapped
Slot: A1
Entry: 17 Oct 2026 15:10:40
Fare: INR 20 per started hour
```

Receipt:

```
PARKING RECEIPT - Ticket #1006
Name: Rahul
Card: A1B2C3D4
Slot: A1
Entry: 17 Oct 2026 14:02:11
Exit: 17 Oct 2026 14:05:26
Duration: 3 min 15 s
Billable hours: 1
Fee: INR 20
```

## Hardware

| Part                                  | Qty    | Purpose                                      |
|---------------------------------------|--------|----------------------------------------------|
| ESP32 dev board                       | 1      | Sensing, RFID, WiFi, Telegram                |
| HC-SR04 ultrasonic sensor             | 1      | Distance measurement                         |
| RC522 RFID reader module (13.56 MHz)  | 1      | Reads driver cards                           |
| MIFARE RFID card or keychain tag      | 1+     | Driver identification (one per registered driver) |
| Raspberry Pi Pico 2 W                 | 1      | LED status indicator                         |
| Red LED and green LED                 | 1 each | Occupied and free indicators                 |
| 220 Ω resistors                       | 2      | LED current limiting                         |
| Jumper wires                          |        | Wiring (female-to-female recommended)        |
| Breadboard (optional)                 | 1      | Prototyping the LED side                     |
| 3D-printed housing (optional)         | 1      | Holds the sensor and the reader              |

## Wiring

All connections below are on the ESP32 unless stated otherwise.

**HC-SR04 to ESP32**

| HC-SR04 | ESP32  |
|---------|--------|
| VCC     | 3V3    |
| GND     | GND    |
| TRIG    | GPIO5  |
| ECHO    | GPIO18 |

**RC522 RFID reader to ESP32**

| RC522      | ESP32                |
|------------|----------------------|
| 3.3V       | 3V3 (**not 5V**)     |
| GND        | GND                  |
| SDA (SS)   | GPIO32               |
| SCK        | GPIO25               |
| MOSI       | GPIO27               |
| MISO       | GPIO26               |
| RST        | GPIO33               |
| IRQ        | not connected        |

> The RC522 is a 3.3 V device. Powering it from 5 V will damage it.
>
> The RC522 uses custom SPI pins on purpose. The ESP32's default SPI pins (GPIO18 and GPIO23) are already used by the ultrasonic sensor and the Pico line.

**ESP32 to Pico 2 W**

| ESP32  | Pico 2 W                        |
|--------|---------------------------------|
| GPIO23 | GP15 (input, HIGH = occupied)   |
| GND    | GND (shared ground is required) |

**LEDs on the Pico 2 W**

| Pico pin | Connection                                           |
|----------|------------------------------------------------------|
| GP13     | 220 Ω resistor, then green LED anode. Cathode to GND |
| GP14     | 220 Ω resistor, then red LED anode. Cathode to GND   |

**ESP32 pin summary**

| ESP32 pin | Used for                  |
|-----------|---------------------------|
| GPIO5     | HC-SR04 TRIG              |
| GPIO18    | HC-SR04 ECHO              |
| GPIO23    | Status line to the Pico   |
| GPIO25    | RC522 SCK                 |
| GPIO26    | RC522 MISO                |
| GPIO27    | RC522 MOSI                |
| GPIO32    | RC522 SDA (SS)            |
| GPIO33    | RC522 RST                 |

All boards use 3.3 V logic, so no level shifting is needed.

> Breadboard contacts can wear out and cause random "no echo" readings or missed card reads. If readings look unstable, connect the sensor and the reader directly to the ESP32 pins with female-to-female jumper wires.

## Software setup

1. Install the Arduino IDE.
2. Add board support:

   - ESP32: the Espressif **esp32** board package
   - Pico 2 W: **Raspberry Pi Pico/RP2040/RP2350** by Earle F. Philhower, III. Add this URL under *Additional boards manager URLs*: `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`

3. Install these libraries from the Library Manager:

   - **WiFiManager** by tzapu
   - **ArduinoJson** (version 7 or newer)
   - **MFRC522** by GithubCommunity (not *MFRC522v2*, which has a different API)

4. Create a Telegram bot with [@BotFather](https://t.me/BotFather) and note the bot token. Get your chat ID by messaging the bot and opening `https://api.telegram.org/bot<TOKEN>/getUpdates`. This is the **admin chat**. Every driver who should get their own messages must also open the bot and press **Start** (or send any message). Their chat ID appears in the same `getUpdates` page.
5. Open `esp32_parking.ino` and set the setup portal password:

   ```
   const char* AP_PASSWORD = "change-this-password";   // min. 8 characters
   ```

   Leave `BOT_TOKEN` and `CHAT_ID` as placeholders. You enter the real values in the portal (next section), so they never go into Git.
6. Upload the sketch to the ESP32. The Arduino IDE will offer to move the file into a folder named `esp32_parking`. Accept.
7. Open `pico_indicator.ino`, select **Raspberry Pi Pico 2 W**, and upload it to the Pico.

### First boot and configuration

1. On first boot (or when no WiFi is saved) the ESP32 opens a WiFi access point named **Parking-Setup**. Connect to it with your phone, using the `AP_PASSWORD` you set.
2. The setup page opens. Choose your 2.4 GHz WiFi network, enter its password, and fill in the parking settings: slot, user name, Telegram bot token, Telegram chat ID, thresholds, confirmation time, and fare. The user name is only a default label. Tickets use the name of the tapped RFID card, or "Guest".
3. Save. The ESP32 connects, stores the settings in flash, and starts monitoring.
4. Open the Serial Monitor at **115200 baud**. The boot output should include `RC522 firmware version: 0x92 (OK)`. `0x91` is also fine. `0x00` or `0xFF` means the reader isn't wired correctly, and the system falls back to guest tickets.

> **Never commit your real WiFi password, bot token, or chat ID.** If a token leaks, revoke it in BotFather with `/revoke`.

### Serial commands

Open the Serial Monitor at 115200 baud, type the letter, and press Enter.

| Command | What it does                                                |
|---------|-------------------------------------------------------------|
| `c`     | Reopen the setup portal                                     |
| `s`     | Print the current settings and the RFID reader status       |
| `r`     | Register a card: tap it, then type the owner name and chat ID |
| `l`     | List the registered cards and their chat IDs                |
| `g`     | Set the guest chat ID for unregistered cards                |
| `x`     | Delete all registered cards (you must type `YES` to confirm)|

### Registering RFID cards

1. Send `r` in the Serial Monitor.
2. Tap the card on the reader within 20 s. The card's UID is printed.
3. Type the owner's name (up to 24 characters) and press Enter within 30 s.
4. Type the owner's Telegram chat ID and press Enter, or type `-` to send that card's messages to the admin chat.
5. The card is saved in flash. Registering the same card again with `r` updates its name and chat ID.

### Where messages go

| Who taps | Ticket and receipt go to |
|---|---|
| A registered card | That card's own Telegram chat |
| A registered card with no chat ID | The admin chat (the one set in the portal) |
| An unregistered card, or no tap | The guest chat set with `g` (the admin chat if none is set) |

If a message can't be delivered (for example the driver never pressed Start on the bot), it is sent to the admin chat instead, marked as not delivered.

Set the guest chat once from the Serial Monitor: send `g`, then type the chat ID of the account you want to use for unregistered cards (type `-` to clear it).

Up to 10 cards can be stored. Use cards or tags that operate at **13.56 MHz** (MIFARE). 125 kHz tags and most phones will not work.

### Uploading to the Pico on Linux with the Flatpak Arduino IDE

The Flatpak sandbox has no `udisksctl`, so the IDE can compile but not copy the file. Instead, compile with *Verify*, hold **BOOTSEL** while plugging in the Pico, and copy the `.uf2` file onto the `RP2350` drive.

If the Serial Monitor shows "Permission denied" on `/dev/ttyUSB0`, add your user to the serial group with `sudo usermod -aG dialout $USER`, then log out and back in.

## How it works

1. The ESP32 connects to WiFi (through the portal on first run), syncs its clock over NTP, and checks that the RC522 reader responds.
2. Every 200 ms it measures the distance to the nearest object. A reading with no echo is ignored, not treated as an empty slot, and readings below 2 cm are rejected as glitches.
3. If the distance stays below the arrival threshold for the confirmation time, a car is parked. The ESP32 sets GPIO23 HIGH (red LED on the Pico), saves the occupied flag and entry time, and starts a 30 s card window.
4. The driver taps a card on the RC522. A registered card issues the ticket with the owner's name and card ID, sent to that owner's Telegram chat. An unregistered card, or no tap before the window ends, issues a "Guest" ticket. The ticket counter is incremented and saved only when the ticket is issued.
5. If the car leaves before the ticket is issued, no ticket or receipt is sent.
6. Once parked, the car only counts as gone when the distance rises above the departure threshold for 2 s. The gap between the two thresholds is the hysteresis.
7. On departure, the ESP32 sets GPIO23 LOW (green LED), calculates the fee, and sends the receipt with the same owner and card as the ticket.
8. The Pico reads GPIO15 and switches between the green and red LEDs.

## Default settings

| Setting                | Default   | Meaning                                              |
|------------------------|-----------|------------------------------------------------------|
| Arrival threshold      | 10 cm     | Closer than this means a car arrives                 |
| Departure threshold    | 15 cm     | Farther than this means the car left                 |
| Arrival confirmation   | 5000 ms   | Time a change must hold before it counts             |
| Departure confirmation | 2000 ms   | `DEPART_CONFIRM_MS` in the sketch                    |
| Card window            | 30 s      | `CARD_WINDOW_MS`: time to tap after arrival          |
| Minimum valid reading  | 2 cm      | `MIN_VALID_CM`: smaller readings are ignored         |
| Maximum cards          | 10        | `MAX_CARDS`                                          |
| Fare                   | INR 20    | Per started hour                                     |
| Time zone              | UTC+5:30  | `GMT_OFFSET_SEC` in the sketch                       |

Raise the two thresholds, keeping a gap of a few cm, for a real slot. The thresholds, arrival confirmation time and fare can be changed in the setup portal. The other values are constants at the top of the sketch.

## Known limitations

- It detects any solid object, not specifically cars. The RFID card identifies the driver but is not checked against the vehicle.
- A card UID can be copied, so RFID here is identification for a demo, not secure authentication. Anyone with a registered card can tap it.
- The RC522 reads only within about 3 cm, so it must be mounted where the driver can tap a card by hand.
- Cards are registered through the Serial Monitor (max 10), and the UID, name and chat ID are stored unencrypted in the ESP32's flash.
- A driver's own Telegram messages only work after they have pressed Start on the bot. Otherwise Telegram rejects the message (it appears in the Serial log) and the message is sent to the admin chat instead.
- The ticket is sent only after the card tap or after the 30 s window, so it can arrive up to 30 s after the car is detected.
- Without a surface in front of the sensor, an empty slot gives no echo, and no-echo readings are ignored. Departure can then be delayed until a valid far reading comes back.
- The HTTPS connection uses `setInsecure()`, which skips certificate validation. This is fine for a demo, but a real deployment should pin the Telegram certificate.
- The bot token is stored in the ESP32's flash without encryption.
- The HC-SR04 is not weatherproof, and at 3.3 V some units give unstable readings. Loose breadboard contacts also cause intermittent no-echo readings.

## Possible next steps

- Add more slot nodes (the Pico 2 W can run a second sensor over WiFi)
- Check the same card again on exit before issuing the receipt
- Register cards with a Telegram `/add` command instead of the Serial Monitor
- Alert the admin when an unregistered card is tapped
- A `/status` bot command showing free slots
- A small server for ticket storage
- A waterproof sensor (JSN-SR04T) for outdoor use

