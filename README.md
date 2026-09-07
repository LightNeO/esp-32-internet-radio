# ESP32-S3 Stereo Wi-Fi Internet Radio

[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue.svg)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/Framework-Arduino-green.svg)](https://www.arduino.cc/)
[![IDE](https://img.shields.io/badge/IDE-PlatformIO-orange.svg)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-MIT-brightgreen.svg)](LICENSE)

An open-source desktop Internet Radio built on the **ESP32-S3 DevKitC-1 (N16R8)**. Features lossless digital I2S audio via dual **MAX98357A** DAC/amplifiers, an animated 0.96" I2C OLED display with word-wrapped station titles and a real-time mini equalizer, a tactile 4-button control layout with short/long press gestures, and a built-in mobile/desktop Web UI with REST API control over your local network.

---
![ESP-32-S3_1](images/1.png)
## Features

- **Zero-Code Wi-Fi Setup (Captive Portal)**: Built-in `WiFiManager` allows anyone to flash pre-built binaries and configure their local Wi-Fi from any smartphone or PC via a captive web portal without touching code. Credentials are saved permanently to NVS flash.
- **Digital Audio**: Uses hardware I2S protocol (`ESP32-audioI2S`) driving MAX98357A Class-D mono/stereo amplifiers with zero analog line hiss.
- **Dynamic Station List**: Supports an unlimited circular station list. Reaching the end of the list automatically wraps back to the beginning.
- **OLED UI (SSD1306 128x64)**:
  - Large text with smart word-boundary wrapping (no words cut off in half).
  - Smooth 8-bar animated equalizer bar visualizer below station titles.
- **Tactile Keypad Navigation**:
  - Button 1: **Short Press** = Previous Station | **Long Press (>800ms)** = Play / Pause
  - Button 2: **Short Press** = Next Station | **Long Press** = Extensible (stub ready in code)
  - Button 3: **Short Press** = Volume Down (`-`) | **Long Press** = Extensible (stub ready in code)
  - Button 4: **Short Press** = Volume Up (`+`) | **Long Press** = Extensible (stub ready in code)
- **Local Web Controller (Port 80)**:
  - Accessible from any smartphone or PC browser via `http://<ESP32-IP>/`.
  - Live status indicator (playing/stopped, current station, volume level).
  - Interactive playback buttons and dynamic one-click station selector.
- **Custom 3D-Printed Enclosure**: Tailored housing designed specifically for this electronics layout.

---

## Hardware & Pinout

### Components Required
| Component | Details | Quantity |
|---|---|---|
| **Microcontroller** | ESP32-S3 DevKitC-1 N16R8 (16MB Flash, 8MB Octal PSRAM) | 1 |
| **I2S DAC / Amplifier** | MAX98357A Class-D (3.2W into 4Ω, 5V or 3.3V) | 1 or 2 (stereo) |
| **Display** | 0.96" Monochrome OLED SSD1306 (128x64, I2C) | 1 |
| **Keypad** | 4-button tactile switch module (common GND) | 1 |
| **Speakers** | 4Ω or 8Ω mini speakers (2W - 5W) | 1 or 2 |
| **Power Supply** | 5V USB-C cable / power bank / 5V DC adapter | 1 |
*You can use ESP-32 3.3v power but 5V separate power is better

---

### Wiring Diagram

```
           +-----------------------------+
           |   ESP32-S3 DevKitC-1 N16R8  |
           +-----------------------------+
                    |   |   |   |
  [I2S Audio]       |   |   |   |
  GPIO 7  (DOUT) ---+   |   |   |----> MAX98357A DIN  (Both Modules)
  GPIO 15 (BCLK) -------+   |   |----> MAX98357A BCLK (Both Modules)
  GPIO 16 (LRC) ------------+   |----> MAX98357A LRC  (Both Modules)
  3.3V or 5V -------------------+----> MAX98357A VIN  (Use 5V only with resistor)
  GND -------------------------------> MAX98357A GND

  [I2C OLED SSD1306]
  GPIO 1 (SDA)  ---------------------> OLED SDA
  GPIO 2 (SCL)  ---------------------> OLED SCL
  3.3V ------------------------------> OLED VCC
  GND -------------------------------> OLED GND

  [4-Button Keypad]
  GPIO 8  ---------------------------> Button 1 (Short: Prev Station, Long: Play/Pause)
  GPIO 9  ---------------------------> Button 2 (Short: Next Station, Long: Extensible)
  GPIO 10 ---------------------------> Button 3 (Short: Volume -,      Long: Extensible)
  GPIO 11 ---------------------------> Button 4 (Short: Volume +,      Long: Extensible)
  GND -------------------------------> Keypad Common GND
```

#### Dual MAX98357A Stereo Configuration
Both amplifier modules share the `BCLK`, `LRC`, and `DIN` pins. Channel separation is configured via the `SD_MODE` pin on each module:
- **Left Channel (Module 1)**: Connect a `~100kΩ` pull-down resistor between `SD_MODE` and `GND`.
- **Right Channel (Module 2)**: Connect a `~100kΩ` pull-up resistor between `SD_MODE` and `3.3V`.
- *(Mono on both speakers: Tie both `SD_MODE` pins to `3.3V` directly)*.

---

## Repository Structure

```text
ESP32-S3-WiFi-Radio/
├── prebuilt-firmware/       # Ready-to-flash binary files (no IDE needed)
│   ├── firmware.bin         # Application firmware (v2.5)
│   ├── bootloader.bin       # ESP32-S3 second-stage bootloader
│   ├── partitions.bin       # Partition table (huge_app layout)
│   └── flash_firmware.bat   # 1-click Windows flashing script
├── source/                  # Full PlatformIO / Arduino source code
│   ├── platformio.ini       # PlatformIO project configuration & dependencies
│   └── src/
│       └── wifi_radio.ino   # Main sketch (stations, buttons, OLED, Web UI)
├── 3d-models/               # 3D printable STL files for the radio body
│   └── README.md
├── .gitignore
└── README.md
```

---

## Quick Start (Flashing Pre-built Firmware)

If you just want to get the radio running without setting up PlatformIO or editing code:

1. Connect your ESP32-S3 board to your PC via a USB-C data cable.
2. Identify your COM port (e.g., `COM3`) in Windows Device Manager.
3. Open `prebuilt-firmware/` and double-click `flash_firmware.bat` (or run via terminal):
   ```bash
   python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x0000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
   ```
4. **First-Time Wi-Fi Setup (Zero Coding)**:
   - On first boot (or if your router is not found), the OLED displays:
     ```text
     WiFi Setup Mode
     Connect to Wi-Fi AP:
     > ESP32-Radio-Setup
     Then open browser:
     http://192.168.4.1
     ```
   - Connect to the Wi-Fi network named **`ESP32-Radio-Setup`** from your phone or PC.
   - The captive setup portal will open automatically (or navigate to `http://192.168.4.1`).
   - Select your home Wi-Fi SSID, enter your password, and click **Save**.
   - The ESP32 saves your network credentials to persistent flash memory, reboots, and automatically starts streaming the radio!

---

## Customizing & Compiling from Source

### Prerequisites
- [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension installed.

### 1. Configure Wi-Fi Credentials
Open `source/src/wifi_radio.ino` and replace the placeholders:
```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

### 2. Customize Radio Stations
You can add as many stations as you want. Simply add pairs of names and direct stream URLs:
```cpp
const char* stationNames[] = {
  "Radio ROKS",
  "KISS FM Ukraine",
  "Lux FM",
  "Hit FM",
  "Lofi Beats",
  "Your Custom Station"   // <-- Add your station name
};

const char* stationUrls[] = {
  "http://online.radioroks.ua/RadioROKS",
  "https://online.kissfm.ua/KissFM_Ukr_HD",
  "https://lux.radio.tvstitch.com/lux_lviv_adv_sd",
  "http://online.hitfm.ua/HitFM",
  "http://das-sa39.cdnstream1.com/5582_128",
  "http://your-stream-url.mp3" // <-- Direct MP3 or AAC stream URL
};
```
The firmware calculates `STATION_COUNT` automatically and loops continuously.

### 3. Build & Flash
In PlatformIO:
1. Click **PlatformIO: Build** (`Ctrl+Alt+B` or checkmark icon in bottom bar).
2. Connect your ESP32-S3 and click **PlatformIO: Upload** (`Ctrl+Alt+U` or arrow icon).
3. Open **PlatformIO: Serial Monitor** at `115200 baud` to see Wi-Fi status and the assigned IP address.

---

## Web Interface

Once connected to your home Wi-Fi network, the ESP32 prints its local IP address to both the Serial Monitor and the initial connection log.

Open your browser and navigate to:
```text
http://<ESP32-IP>/
```

### Available REST Endpoints:
- `GET /api/status` — Returns JSON with playback state, volume, active station, and station array.
- `GET /api/toggle` — Toggles Play / Stop.
- `GET /api/next` — Plays the next station.
- `GET /api/prev` — Plays the previous station.
- `GET /api/vup` — Increases volume by 1 (max 21).
- `GET /api/vdown` — Decreases volume by 1 (min 0).
- `GET /api/station/<id>` — Jumps directly to station index `<id>`.

---

## 3D Enclosure Design

The custom housing is located under the `3d-models/ESP-32-radio.3mf`. Optimized for FDM 3D printing (0.2mm layer height, PETG or PLA, 15% infill).
It is still not perfect desicion. Depending on your wiring, speakers etc you might need some glue and screws

---

## License

This project is licensed under the MIT License - feel free to build, modify, and share!
