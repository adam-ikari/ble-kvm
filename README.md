# BLE-KVM

[English](README.md) | [中文](README_CN.md)

Bluetooth Low Energy KVM (Keyboard-Video-Mouse) switch for ESP32-S3. Connects to up to three PCs simultaneously — two via BLE HID and one via USB HID device mode. Switch active PC with a button press. Supports USB device mode, USB host mode, WiFi web configuration, and cloud voice-to-text input.

## Supported Boards

| Board | HAS_VOICE | HAS_TFT | HAS_BATTERY | HAS_USB | HAS_IMU | LED |
|-------|-----------|---------|-------------|---------|---------|-----|
| **M5StickS3** | ✅ | ✅ | ✅ | ✅ | ✅ | None (GPU backlight) |
| **M5StampS3** | ❌ | ❌ | ❌ | ✅ | ❌ | RGB (WS2812) |
| **Generic ESP32-S3** | ❌ | ❌ | ❌ | ❌ | ❌ | GPIO LEDs |

## Features

- **BLE HID KVM** — Connect up to 2 PCs via Bluetooth BLE HID. A 3rd PC can be connected via USB device mode. Keyboard and mouse input transparently routed to the active connection.
- **USB Device Mode** — Appears as a USB HID keyboard/mouse to a connected host via the USB OTG port.
- **USB Host Mode** — Accepts external USB HID keyboard/mouse, forwarding their input over BLE to connected PCs.

### M5StickS3 Only

- **Cloud Voice Input** — Long-press the primary button to record speech. Audio streams to [Baidu Real-Time ASR](https://ai.baidu.com/tech/speech/asr) via WebSocket. Recognized text is typed as HID keyboard input on the active PC.
- **TFT Display** — 135×240 color display shows active PC, connection status, battery level, input mode, voice recording indicator, and factory reset warning.
- **IMU Air Mouse** — 6-axis IMU enables air mouse / presentation pointer mode. Cycle input modes with the secondary button.
- **Battery Monitoring** — Real-time battery percentage and charging status (AXP2101 PMIC).

### All Boards

- **WiFi Web Configuration** — Built-in web server (STA or AP mode) for configuring WiFi, paired PCs, USB mode, anti-idle, voice ASR settings, and more. Protected by a configurable auth token.
- **Two-Button UI** — Primary button: short-press switches PCs, long-press starts voice input. Secondary button: short-press cycles input modes, 5s-long-press warns factory reset, 10s executes.
- **Anti-Idle** — Periodic HID keep-alive signals to prevent host sleep/lock.
- **Web Debug Log** — Real-time ESP-IDF log streaming over SSE on the web dashboard. Disabled by default, toggle in settings.
- **Factory Reset** — Via secondary button (10s hold) or `POST /api/factory-reset` with `{"confirm": true}`. Erases all NVS settings and reboots.

## Prerequisites

- **ESP-IDF v5.5** — [Installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
- **Python 3.10+**
- **ESP32-S3 board** — M5StickS3 recommended for full feature set

### ESP-IDF Component Dependencies

Defined in `main/idf_component.yml`:

| Component | Version | Purpose |
|-----------|---------|---------|
| `espressif/esp_tinyusb` | ^2.0.0 | USB device stack |
| `espressif/usb_host_hid` | ^1.0.1 | USB host HID driver |
| `espressif/esp_websocket_client` | ^1.7.0 | WebSocket client for voice ASR |

### M5StickS3 I2C Bus

The M5StickS3 uses a shared I2C bus (GPIO47=SDA, GPIO48=SCL) with three peripherals:

| Device | I2C Address | Driver |
|--------|------------|--------|
| AXP2101 PMIC | 0x6E | `power_manager.c` |
| BMI270 IMU | 0x68 | `imu_driver.c` |
| ES8311 Audio Codec | 0x18 | `es8311_driver.c` |

## Build and Flash

### 1. Clone and initialize components

```bash
git clone <repo-url> && cd ble-kvm
```

The `managed_components/` directory is gitignored — ESP-IDF fetches dependencies at build time.

### 2. Set up ESP-IDF environment

```bash
. $HOME/esp/esp-idf/export.sh
```

### 3. Build for your board

```bash
# M5StickS3 (full features)
BOARD=m5sticks3 idf.py build

# M5StampS3 (BLE + USB + RGB LED)
BOARD=m5stamps3 idf.py build

# Generic ESP32-S3 (BLE + GPIO LEDs)
BOARD=default idf.py build
```

You can set `BOARD` in your environment to avoid passing it each time:

```bash
export BOARD=m5sticks3
```

### 4. Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

## Project Structure

```
ble-kvm/
├── main/                    # ESP-IDF main component
│   ├── main.c               # App entry point, initialization order
│   ├── board.h              # Board definitions, pin maps, feature flags
│   ├── config_manager.h/c   # NVS-backed configuration (WiFi, PCs, voice, etc.)
│   ├── ble_peripheral.h/c   # BLE HID peripheral advertising and connections
│   ├── ble_central.h/c      # BLE central scanning for input devices
│   ├── hid_router.h/c       # Keyboard/mouse report routing to active PC
│   ├── switch_manager.h/c   # Button handling, PC switching, voice/factory triggers
│   ├── wifi_manager.h/c     # WiFi AP + STA management
│   ├── web_server.h/c       # HTTP API + SSE + embedded web dashboard
│   ├── web_log.h/c          # vprintf hook → SSE log broadcast
│   ├── indicator.h/c        # LED/TFT state management
│   ├── anti_idle.h/c        # Periodic HID keep-alive
│   ├── gpio_led.h/c         # GPIO LED driver (generic board)
│   ├── rgb_led.h/c          # WS2812 RGB LED driver (StampS3)
│   ├── tft_display.h/c      # ST7789 TFT display (M5StickS3)
│   ├── power_manager.h/c    # AXP2101 PMIC battery/charging (M5StickS3)
│   ├── input_mode.h/c       # KVM ↔ Air Mouse mode switching (M5StickS3)
│   ├── imu_driver.h/c       # BMI270 IMU driver (M5StickS3)
│   ├── es8311_driver.h/c    # ES8311 audio codec driver (M5StickS3)
│   ├── mic_driver.h/c       # I2S MEMS microphone driver (M5StickS3)
│   ├── voice_input.h/c      # Voice recording + Baidu ASR WebSocket client
│   ├── usb_device.h/c       # TinyUSB HID device
│   ├── usb_host.h/c         # USB host HID driver
│   ├── CMakeLists.txt       # Component build (board-conditional sources)
│   └── idf_component.yml    # ESP-IDF component dependencies
├── web/                     # React web dashboard (Vite + TypeScript)
│   ├── src/
│   ├── index.html
│   ├── vite.config.ts
│   └── package.json
├── CMakeLists.txt           # Root project definition
├── sdkconfig                # Default SDK configuration
├── dependencies.lock        # ESP-IDF component lock file
└── .gitignore
```

## Configuration

All settings are persisted in NVS flash and configurable via the web dashboard at `http://<device-ip>/`.

| Setting | Default | Description |
|---------|---------|-------------|
| `auth_token` | 9-char random | Web UI authentication token |
| `wifi_ssid` / `wifi_password` | — | STA mode credentials |
| `wifi_enabled` | false | Enable WiFi on boot |
| `usb_mode` | 0 (disabled) | 0=BLE only (2 PCs), 1=USB device (adds 3rd PC), 2=USB host |
| `active_pc` | 0 | Currently selected PC (0-1 BLE, 2 USB device) |
| `anti_idle_enabled` | false | Send periodic HID keep-alive |
| `anti_idle_interval_sec` | — | Interval between keep-alive signals |
| `input_mode` | 0 (KVM) | 0=KVM, 1=PPT air mouse |
| `air_mouse_sensitivity` | 5 | IMU sensitivity (1-10) |
| `voice_asr_enabled` | false | Enable voice input feature |
| `voice_asr_appid` | 0 | Baidu ASR App ID |
| `voice_asr_api_key` | — | Baidu ASR API Key (appkey) |
| `voice_lang` | "zh" | ASR language: "zh" or "en" |
| `voice_input_mode` | 0 (auto) | Text input: 0=auto, 1=pinyin, 2=ASCII only |

## Web API

The web server exposes a REST API (all endpoints require `Authorization: Bearer <token>` header):

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | Device status (PCs, batteries, connections) |
| `GET` | `/api/settings` | All configuration values |
| `PATCH` | `/api/settings` | Update configuration |
| `POST` | `/api/switch` | Switch active PC |
| `POST` | `/api/factory-reset` | Erase NVS and reboot (`{"confirm": true}`) |
| `GET` | `/api/logs` | SSE stream of real-time ESP-IDF logs |

## Voice Input (M5StickS3)

### How it works

1. Long-press the primary button (≥500ms) — the TFT turns solid red
2. Speak into the built-in microphone
3. Release the button → sends FINISH frame to Baidu ASR
4. Recognized text is typed as HID keyboard scan codes on the active PC

### Audio path

```
SPM1423 MEMS Mic (analog) → ES8311 ADC → I2S → ESP32-S3
                                              ↓
                              Baidu Real-Time ASR ← WebSocket ← PCM 16kHz/16bit/mono
```

### Baidu ASR Setup

1. Register at [Baidu AI Cloud](https://console.bce.baidu.com/ai/)
2. Create a "Real-Time Speech Recognition" application
3. Note the **App ID** and **API Key**
4. Enter them in the web dashboard Settings page

### Text input modes

- **Auto (0)**: Best-effort — ASCII characters are typed directly; CJK characters are skipped (requires PC-side Chinese IME for Chinese output)
- **Pinyin (1)**: Same as auto; CJK input requires an active Chinese IME on the target PC
- **ASCII (2)**: Only type ASCII characters, skip all non-ASCII bytes

> **Note:** HID keyboard reports can only send USB HID keycodes (US layout). CJK characters require the target PC to have a Chinese IME active. The device types Pinyin romanization as ASCII keystrokes; the IME converts it to Chinese characters.

## Factory Reset

**Method 1 — Button (M5StickS3):** Hold the secondary button for 5 seconds (TFT shows warning), continue holding for another 5 seconds (10s total) to execute.

**Method 2 — Web API:** `POST /api/factory-reset` with body `{"confirm": true}` and valid auth token.

Both methods erase all NVS data and reboot the device.

## License

MIT
