# Smart-Parking-Slot-Monitor
ESP32 + HC-SR04 parking slot monitor. Detects cars with a 5 s filter and hysteresis, then sends NTP-timestamped tickets and per-hour fee receipts to a Telegram bot over HTTPS. A Pico 2 W shows slot status on red/green LEDs, and ticket numbers persist in flash (NVS). Built in C++ on Arduino.
