# Voice Input & Factory Reset Redesign

**Goal:** Add voice-to-keyboard input via M5StickS3 built-in microphone and cloud ASR; move factory reset from primary button 5s-long-press to secondary button with 2-stage confirmation.

**Architecture:** Primary button press starts I2S recording via ES8311 codec + SPM1423 mic, audio is streamed via WebSocket to Baidu Real-Time ASR. Recognized text is sent as HID keyboard input (pinyin for Chinese, direct for ASCII) to the active PC. Factory reset moves to secondary button: 5s shows warning, 10s confirms and executes.

**Tech Stack:** ESP-IDF I2S driver (ES8311 codec + SPM1423 mic), esp_websocket_client (Baidu Real-Time ASR), cJSON, HID keyboard report generation.

---

## Button Behavior Changes

### Primary Button (GPIO 11)

| Duration | Old | New |
|----------|-----|-----|
| <50ms | (debounce) | (debounce) |
| 50ms-500ms | CMD_SWITCH (cycle PC) | CMD_SWITCH (cycle PC) |
| >500ms (held) | — | CMD_VOICE_START (begin recording) |
| On release after >500ms | CMD_FACTORY_RST if >5s | CMD_VOICE_STOP (stop recording, finalize ASR) |
| 5s+ | CMD_FACTORY_RST | Continue recording (no factory reset) |

On release after long press: if voice ASR is enabled + WiFi connected → stop recording and finalize ASR. If voice ASR disabled or no WiFi → no action (short press still works as CMD_SWITCH).

### Secondary Button (GPIO 12)

| Duration | Old | New |
|----------|-----|-----|
| 50ms-1s | CMD_SECONDARY | CMD_SECONDARY (same) |
| 1s-3s | CMD_MODE_CYCLE | CMD_MODE_CYCLE (same) |
| 3s-5s | (none) | (none) |
| 5s | (none) | Show TFT warning: "Continue holding for factory reset" |
| 10s | (none) | CMD_FACTORY_RST (execute) |
| Release between 5s-10s | — | Cancel, clear warning |

**Two-stage confirmation**: At 5s, TFT shows warning message. User must keep holding until 10s for actual reset. Releasing between 5s-10s cancels. On boards without TFT (StampS3), LED flashes rapidly as warning indicator from 5s to 10s.

### Boards without secondary button (StampS3, DevKitC)

Factory reset only via web UI. No button-triggered factory reset on single-button boards. The web settings page will have a "Factory Reset" button with confirmation dialog.

---

## Voice Input Architecture

### Hardware: M5StickS3 Audio Path

M5StickS3 does NOT connect SPM1423 directly via I2S. The actual audio path is:

```
SPM1423 (analog MEMS mic) → ES8311 (audio codec, I2C addr 0x18) → I2S → ESP32-S3
```

- **ES8311 codec** handles ADC, gain, and I2S formatting
- ES8311 shares the I2C bus with IMU (0x68) and PMIC (0x6E): SDA=GPIO47, SCL=GPIO48
- I2S pins: MCLK=GPIO18, BCLK=GPIO17, WS=GPIO15, DATA_IN=GPIO16
- I2S port: I2S_NUM_1 (port 1, not port 0)
- 8MB PSRAM available on M5StickS3

### Components

1. **`es8311_driver.h/c`** — ES8311 audio codec driver
   - `es8311_init(i2c_port_t)` — Initialize codec via I2C (set clock, ADC power up, mic input select, gain)
   - `es8311_set_mic_gain(uint8_t gain)` — Set microphone gain (0-14, step 3dB)
   - `es8311_deinit()` — Power down codec
   - Board-conditional: only compiled for BOARD_M5STICKS3 (uses `#if HAS_VOICE_INPUT`)

2. **`mic_driver.h/c`** — I2S recording driver
   - `mic_driver_init()` — Configure I2S port 1 (16kHz, 16-bit, mono, DMA buffers)
   - `mic_driver_start()` — Start I2S capture (call after es8311_init)
   - `mic_driver_stop()` — Stop I2S capture
   - `mic_driver_read(buf, len, timeout)` — Read PCM samples from I2S DMA buffer
   - Board-conditional: only compiled for BOARD_M5STICKS3 (uses `#if HAS_VOICE_INPUT`)

3. **`voice_input.h/c`** — Voice input manager
   - `voice_input_init()` — Initialize state
   - `voice_input_start()` — Open WebSocket to Baidu ASR, start streaming
   - `voice_input_stop()` — Send FINISH frame, wait for final result, send keyboard input
   - `voice_input_cancel()` — Send CANCEL frame, abort recording and ASR
   - `voice_input_is_active()` — Returns true while recording
   - Internal: WebSocket send task reads PCM from mic_driver, sends binary frames
   - Internal: WebSocket receive callback parses ASR JSON results
   - Internal: text_to_keyboard() converts recognized text to HID key sequences

4. **`switch_manager.c`** — Button behavior refactored (changes to existing file)
   - Primary button ISR: send CMD_VOICE_START on >500ms held, CMD_VOICE_STOP on release after long press
   - Secondary button ISR: track duration milestones at 5s (show warning) and 10s (factory reset)
   - Command processing: CMD_VOICE_START calls voice_input_start(), CMD_VOICE_STOP calls voice_input_stop()

### Data Flow

```
[Primary Button Held >500ms]
  → es8311_init() + mic_driver_start()
  → I2S DMA (I2S_NUM_1) → mic_driver_read() loop (5120 bytes per 160ms)
  → PCM binary frames → WebSocket → Baidu RT-ASR
  → Baidu returns MID_TEXT results (partial, ignored)
  → [Primary Button Released]
  → mic_driver_stop()
  → Send FINISH frame
  → Baidu returns FIN_TEXT result (final recognized text)
  → text_to_keyboard(): ASCII→direct keycode, Chinese→pinyin IME keystrokes
  → hid_router_forward_keyboard() → active PC (BLE or USB)
```

### Baidu Real-Time ASR WebSocket API

- **URL**: `wss://vop.baidu.com/realtime_asr?sn={uuid}`
  - `sn` is a user-defined session ID for log tracing (UUID format recommended)

- **Authentication**: Send `appid` and `appkey` directly in the START frame — no separate token step needed

- **Protocol flow**:
  1. Connect to WebSocket URL
  2. Send START frame (text/JSON) with app config
  3. Send binary frames of PCM audio (~160ms / 5120 bytes each)
  4. Receive MID_TEXT (partial results) and FIN_TEXT (final result) as text/JSON
  5. Send FINISH frame when done
  6. Server sends final result and closes

- **START frame**:
```json
{
  "type": "START",
  "data": {
    "appid": 105xxx17,
    "appkey": "UA4oPSxxxxkGOuFbb6",
    "dev_pid": 15372,
    "cuid": "ble-kvm-sticks3",
    "format": "pcm",
    "sample": 16000
  }
}
```

- **FINISH frame**: `{"type": "FINISH"}`
- **CANCEL frame**: `{"type": "CANCEL"}`

- **Response format**:
```json
{"err_no": 0, "err_msg": "OK", "type": "MID_TEXT", "result": "partial text"}
{"err_no": 0, "err_msg": "OK", "type": "FIN_TEXT", "result": "final complete text"}
```

- **Audio format**: PCM 16kHz 16-bit mono, binary frames, ~5120 bytes per frame (160ms)
- **Timeout**: Server disconnects if no audio received for 5 seconds
- **Language models (dev_pid)**: 15372 = Mandarin Chinese (enhanced punctuation), 1737 = English

- **Error codes**: err_no=0 is success; -3004 = authentication failed; others = various ASR errors

### Text to Keyboard Input Conversion

HID keyboard reports can only send physical key scan codes, not Unicode characters directly. Chinese character input requires using the target PC's Input Method Editor (IME).

**Strategy — configurable input mode (default: pinyin IME):**

1. **Pinyin IME mode** (default for Chinese, requires IME on target PC):
   - For Chinese text: send pinyin letters as standard keycodes, let the target PC's IME convert
   - The ASR result includes the recognized Chinese characters, but we send the pinyin representation
   - After each word's pinyin, send a number key (1-9) to select the top candidate
   - Requires the target PC to have a Chinese IME active (Microsoft Pinyin, Sogou, etc.)
   - Limitation: candidate selection may not always match the intended character
   - Practical for common words/phrases where top candidate is usually correct

2. **ASCII mode** (for English text or mixed input with English IME active):
   - Direct keycode sending for a-z, 0-9, and standard symbols
   - Spaces, punctuation mapped to HID usage codes

3. **Input mode selection**: New config field `voice_input_mode` (0=auto, 1=pinyin, 2=ascii)
   - Auto: use pinyin if voice_lang is "zh", ascii if "en"

**Keystroke timing**: 20ms press+release interval between characters to avoid overrun

**Routing**: If active PC is USB device (PC3), route through `usb_device_send_keyboard()`. If BLE (PC1/PC2), route through BLE HID report.

### Memory Management

- I2S DMA buffer: standard ESP-IDF default (2× 1024 bytes)
- PCM read buffer: 5120 bytes per read (160ms of audio at 16kHz/16bit)
- WebSocket send: streaming, no large accumulation
- ASR result buffer: 256 bytes
- ES8311 I2C commands: negligible
- Total additional RAM: ~10KB (well within ESP32-S3 capability, 8MB PSRAM available)

### WiFi Requirement

Voice input requires WiFi STA mode connected to internet. Checks before starting:
1. `voice_asr_enabled` must be true
2. WiFi STA must be connected (`wifi_manager_is_sta_connected()`)
3. If either condition fails: TFT shows brief message ("Need WiFi"), no recording starts

### Error Handling

- **WebSocket connection failure**: Abort recording, TFT shows "ASR connection failed"
- **No internet / DNS failure**: Abort, TFT shows "Network error"
- **ASR returns error (err_no != 0)**: Log error, TFT shows "ASR error", no keyboard input
- **ASR returns empty result**: No keyboard input sent
- **5-second audio timeout**: Server disconnects if no audio sent for 5s — mic_driver_read ensures continuous streaming
- **Button released before minimum audio**: If recording <200ms, send CANCEL frame, discard result

---

## Configuration (NVS)

### New fields in `kvm_config_t`

```c
bool voice_asr_enabled;           /* default: false */
uint32_t voice_asr_appid;         /* Baidu App ID */
char voice_asr_api_key[65];       /* Baidu API Key (appkey) */
char voice_lang[8];               /* "zh" or "en", default: "zh" */
uint8_t voice_input_mode;         /* 0=auto, 1=pinyin, 2=ascii */
```

Note: Removed `voice_asr_secret` field — Baidu RT-ASR WebSocket API authenticates via appid+appkey in the START frame, no separate secret or token needed.

### NVS keys (namespace: `kvm_config`)

- `voice_en` — uint8 (0/1 for enabled)
- `voice_appid` — uint32 (App ID)
- `voice_ak` — string (API Key / appkey)
- `voice_lang` — string (language code)
- `voice_im` — uint8 (input mode: 0=auto, 1=pinyin, 2=ascii)

### Web API Changes

**GET /api/settings** — add:
- `voice_asr_enabled`: bool
- `voice_asr_appid`: number
- `voice_asr_api_key`: string (masked if set, e.g. "****abcd")
- `voice_lang`: string
- `voice_input_mode`: number

**PATCH /api/settings** — add:
- `voice_asr_enabled`: bool — toggle voice input
- `voice_asr_appid`: number — set App ID
- `voice_asr_api_key`: string — set API Key (full value on write, masked on read)
- `voice_lang`: string — set language ("zh" or "en")
- `voice_input_mode`: number — set input mode

**GET /api/status** — add:
- `voice_recording`: bool — whether voice recording is active

**POST /api/factory-reset** (new endpoint):
- Requires auth
- Body: `{ "confirm": true }`
- Erases NVS, reboots — used by web UI for boards without secondary button

### TFT Display Changes

**Status page**: When `voice_asr_enabled` and recording, show microphone icon and "Recording..." text
**Debug page**: Show voice ASR status (enabled/disabled, connected/not)
**Warning overlay**: When secondary button held 5s, show "Hold 10s for factory reset" in red text. Clear on release or execution.

---

## Board Feature Flags

```c
/* board.h additions */
#if BOARD_M5STICKS3
#define HAS_VOICE_INPUT   1
#define MIC_I2S_PORT      I2S_NUM_1
#define MIC_I2S_MCK_GPIO  GPIO_NUM_18
#define MIC_I2S_BCK_GPIO  GPIO_NUM_17
#define MIC_I2S_WS_GPIO   GPIO_NUM_15
#define MIC_I2S_DATA_GPIO GPIO_NUM_16
#define ES8311_I2C_ADDR   0x18
#else
#define HAS_VOICE_INPUT   0
#endif
```

Note: ES8311 codec shares the I2C bus with IMU (addr 0x68) and PMIC (addr 0x6E) on SDA=GPIO47, SCL=GPIO48. The es8311_driver must use the same I2C port as the IMU driver.

---

## Factory Reset via Web (all boards)

New `POST /api/factory-reset` endpoint available on all boards. The web UI settings page shows a "Factory Reset" button. Clicking it shows a confirmation dialog. If confirmed, sends `{ "confirm": true }`. The handler erases NVS and calls `esp_restart()`.

This is the only factory reset method on boards without a secondary button (StampS3, DevKitC).

---

## Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Create | `es8311_driver.h/c` | ES8311 audio codec driver (I2C init, gain, ADC config) |
| Create | `mic_driver.h/c` | I2S recording driver (I2S_NUM_1, 16kHz/16bit/mono) |
| Create | `voice_input.h/c` | Voice input manager (Baidu ASR WebSocket, text→keyboard) |
| Modify | `switch_manager.c` | Button behavior: primary→voice, secondary→2-stage reset |
| Modify | `config_manager.h/c` | Add voice ASR config fields, NVS load/save |
| Modify | `web_server.c` | Add voice config to settings, voice_recording to status, factory-reset endpoint |
| Modify | `board.h` | Add HAS_VOICE_INPUT, ES8311 + I2S pin definitions |
| Modify | `tft_display.c` | Recording indicator, factory reset warning overlay |
| Modify | `main.c` | Init es8311, mic_driver, and voice_input if enabled |
| Modify | `CMakeLists.txt` | Add es8311_driver.c, mic_driver.c, voice_input.c (conditional on HAS_VOICE_INPUT) |
| Modify | `indicator.h/c` | Add IND_VOICE_RECORDING, IND_FACTORY_WARN states |

---

## Out of Scope

- Local/on-device ASR (ESP32-S3 insufficient compute)
- Voice wake word detection
- Audio playback / speaker output
- Noise cancellation or audio preprocessing beyond basic I2S capture
- Multi-language mixed recognition (single language per session)
- USB microphone input
- Alt+Numpad Unicode input method (requires registry changes on Windows, not practical)
