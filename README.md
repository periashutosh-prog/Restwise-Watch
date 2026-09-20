# ⌚ Restwise

**A low-cost, radio-off wellbeing watch for students.**

Restwise shows one thing: today's schedule -- classes, meals, water breaks, and sleep windows -- synced from a companion website over USB and displayed on a small always-available wrist screen. No app to install, no notifications, no phone required to use it day-to-day. It runs on an ESP32-C6, and keeps WiFi and Bluetooth disabled during normal wear by design.

---

## 📋 Table of Contents

- [✨ Key Features](#-key-features)
- [🔩 Hardware Overview](#-hardware-overview)
- [🔌 Pin Mapping](#-pin-mapping)
- [🧠 Firmware Architecture](#-firmware-architecture)
- [🔄 Restwise Sync Protocol](#-restwise-sync-protocol)
- [📁 Repository Structure](#-repository-structure)
- [🛠️ Building & Flashing](#️-building--flashing)
- [📐 Technical Specifications](#-technical-specifications)
- [⚠️ Safety Notice](#️-safety-notice)
- [📄 License](#-license)

---

## ✨ Key Features

- 🔋 **Radio-off by default** -- WiFi and Bluetooth are explicitly disabled at boot (`WiFi.mode(WIFI_OFF); btStop();`), for continuous low-RF wear rather than as a cost-saving measure
- 🖥️ **Standalone timetable display** -- once synced, the watch shows the day's schedule entirely offline, browsable by day (Today, Monday..Saturday)
- 🔁 **Custom USB serial sync protocol** -- a small line-based handshake pushes a full week's schedule from the companion website to the watch in seconds, no companion app required
- 🕒 **RTC with graceful fallback** -- uses a DS3231 real-time clock when present; falls back to a software clock seeded at boot if the RTC isn't detected, so the watch never halts on a missing component
- 🌙 **Good Night screen** -- a passive night-time greeting with a crescent moon, shown automatically during configured night hours
- 🔐 **PIN lock & app suite** -- Stopwatch, Timer (with alert), Calculator (with a scientific mode), a lock-screen PIN gate, and a USB terminal console, all built on the same screen-state-machine UI
- 🔌 **Dual power path** -- runs from USB, battery, or both at once, via a diode-ORed supply feeding a single 3.3V regulator

---

## 🔩 Hardware Overview

| Component | Part |
|---|---|
| MCU | ESP32-C6-MINI-1-N4 |
| Display | 0.96" OLED, 128x64, SSD1306, I2C |
| RTC | DS3231 (optional at boot, software fallback if absent) |
| Input | 5-button D-pad (Up / Down / Left / Right / Center) |
| Regulator | AP2112K-3.3 LDO |
| Charging | External MCP73831-based charger board (see `PCB Files/`) |
| Storage | Onboard flash (LittleFS) for the synced timetable; EEPROM for settings |

---

## 🔌 Pin Mapping

| Signal | GPIO |
|---|---|
| SCL | 6 |
| SDA | 14 |
| Button -- Up | 23 |
| Button -- Down | 21 |
| Button -- Left | 19 |
| Button -- Right | 20 |
| Button -- Center | 3 |

> ℹ️ Only GPIO0-7 sit in the ESP32-C6's low-power (LP) domain and can wake the chip from *deep* sleep. Center (GPIO3) was deliberately placed in that range in case deep-sleep wake-on-press is added later. Light sleep has no such restriction and works on all five button pins.

All buttons are wired active-low to GND with internal pull-ups enabled in firmware -- no external resistors needed.

---

## 🧠 Firmware Architecture

The UI is a **screen-state machine**: a single `ScreenState` enum value tracks what's on screen at any moment, `drawScreen()` dispatches to the matching draw function, and every screen has its own `if/else` block in `loop()` for input handling. Transitions between screens use a simple offset-based slide animation.

**Screens included:** Watchface, Menu, Restwise (timetable + day view), Stopwatch, Timer (+ alert), Calculator (+ scientific mode), PIN entry, Security menu/confirm, Lock-screen settings, Terminal, and the Good Night screen.

Buttons are polled directly in `loop()` (50ms interval, which also serves as debounce) -- there's no RTOS task reading input separately.

---

## 🔄 Restwise Sync Protocol

A small, line-based ASCII protocol runs over the same USB serial connection used for flashing, active on every screen except the Terminal app (which owns Serial for its own console protocol while active):

```
Host -> ESP   RW_HELLO                          ESP -> Host   RW_HELLO
Host -> ESP   RW_TIME <YYYY-MM-DD HH:MM:SS>      ESP -> Host   RW_TIME_OK
Host -> ESP   RW_BEGIN                           ESP -> Host   RW_READY
Host -> ESP   <days>|<startMin>|<endMin>|<label>  (repeated, one per line)
Host -> ESP   RW_END                             ESP -> Host   RW_OK <count>
```

Receiving a handshake auto-navigates the watch to the Restwise screen, since opening the serial port resets the chip back to the watchface. The received schedule is written straight to `/timetable.rw` on LittleFS and reloaded immediately once the transfer completes -- no reboot required.

---

## 📁 Repository Structure

```
Restwise-watch/
├── Restwise-watch.ino    # Firmware source (this repo's main sketch)
├── PCB Files/            # Schematic, PCB layout, and Gerber/export files
├── README.md             # This file
└── LICENSE                # All Rights Reserved
```

---

## 🛠️ Building & Flashing

1. Install the **ESP32 board package** in Arduino IDE / `arduino-cli` (board: `esp32:esp32:esp32c6`).
2. Install the required libraries: `RTClib`, `Adafruit SSD1306`, `Adafruit GFX Library`.
3. Open `Restwise-watch.ino` and select the ESP32-C6 board + correct COM port.
4. Compile and upload.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c6 .
arduino-cli upload -p COMx --fqbn esp32:esp32:esp32c6 .
```

---

## 📐 Technical Specifications

| Spec | Value |
|---|---|
| MCU | ESP32-C6 |
| Flash usage | ~93% of 1.31MB (default partition scheme) |
| RAM usage | ~14% of 320KB |
| Display | 0.96" OLED, 128x64, I2C |
| Sync interface | USB serial (Web Serial API on the website side) |
| Radio | Off by default |

---

## ⚠️ Safety Notice

This project includes custom battery charging circuitry and a lithium battery. Building or operating a device from these files carries real electrical and fire risk if assembled incorrectly. Anyone building this does so entirely at their own risk. See `LICENSE` for the full disclaimer.

---

## 📄 License

**All Rights Reserved.** See [`LICENSE`](./LICENSE) for full terms. This repository is shared for viewing and evaluation only (e.g. school project review, competition judging, sponsorship review) -- no permission is granted to copy, modify, or redistribute any part of it without prior written consent.
