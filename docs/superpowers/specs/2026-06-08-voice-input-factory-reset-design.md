# Voice Input & Factory Reset Redesign

**Goal:** Add voice-to-keyboard input via M5StickS3 built-in microphone and cloud ASR; move factory reset from primary button 5s-long-press to secondary button with 2-stage confirmation.

**Architecture:** Primary button long-press starts I2S recording from SPM1423 mic, audio is streamed via WebSocket to Baidu Real-Time ASR. Recognized text is sent as HID keyboard input to the active PC. Factory reset moves to secondary button: 5s shows warning, 10s confirms and executes.

**Tech Stack:** ESP-IDF I2S driver (SPM1423), esp_websocket_client (Baidu Real-Time ASR), cJSON, HID keyboard report generation.

---

## Button Behavior Changes

### Primary Button (GPIO 11)

| Duration | Old | New |
|----------|-----|-----|
| <50ms | (debounce) | (debounce) |
| 50ms-500ms | CMD_SWITCH (cycle PC) | CMD_SWITCH (cycle PC) |
| >500ms (held) | — | CMD_VOICE_START (begin recording) |
| On release after >500ms | CMD_FACTORY_RST if >5s | CMD_VOICE_STOP (stop recording, send ASR) |
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

### Components

1. **`mic_driver.h/c`** — I2S driver for SPM1423 MEMS microphone
   - `mic_driver_init()` — Configure I2S port (16kHz, 16-bit, mono)
   - `mic_driver_start()` — Start I2S capture
   - `mic_driver_stop()` — Stop I2S capture
   - `mic_driver_read(buf, len, timeout)` — Read PCM samples from I2S DMA buffer
   - Board-conditional: only compiled for BOARD_M5STICKS3 (uses `#if HAS_VOICE_INPUT`)

2. **`voice_input.h/c`** — Voice input manager
   - `voice_input_init()` — Initialize (read config)
   - `voice_input_start()` — Open WebSocket to Baidu ASR, start streaming
   - `voice_input_stop()` — Send end marker, wait for final result, send keyboard input
   - `voice_input_cancel()` — Abort recording and ASR (e.g., no WiFi, timeout)
   - `voice_input_is_active()` — Returns true while recording
   - Internal: WebSocket receive task parses ASR results, queues final text
   - Internal: Text-to-keyboard function converts Unicode text to HID key sequences

3. **`switch_manager.c`** — Button behavior refactored (changes to existing file)
   - Primary button ISR: track press/release, send CMD_VOICE_START on >500ms held, CMD_VOICE_STOP on release after long press
   - Secondary button ISR: track duration milestones at 5s (show warning) and 10s (factory reset)
   - Command processing: CMD_VOICE_START calls voice_input_start(), CMD_VOICE_STOP calls voice_input_stop()

### Data Flow

```
[Primary Button Held]
  → mic_driver_start() → I2S DMA → mic_driver_read() loop
  → PCM chunks → voice_input WebSocket send task → Baidu RT-ASR
  → Baidu returns partial results (ignored)
  → [Primary Button Released]
  → mic_driver_stop()
  → WebSocket end marker sent
  → Baidu returns final text result
  → voice_input: text → HID keyboard key sequences
  → hid_router_forward_keyboard() → active PC (BLE or USB)
```

### Baidu Real-Time ASR WebSocket API

- **URL**: `wss://vop.baidu.com/pro_api/asr?token={access_token}`
- **Auth**: Obtain access_token via REST API using API Key + Secret Key (cached for 30 days)
- **Audio format**: PCM 16kHz 16-bit mono, sent as binary frames
- **Protocol**: Send audio frames continuously; send empty frame or close connection to finalize
- **Response**: JSON frames with `result` field containing recognized text
- **Final result**: Last JSON frame after end marker contains complete recognized text
- **Language**: Configurable (default: Chinese `zh`), also supports `en` (English)

### Text to Keyboard Input Conversion

Recognized text must be converted to HID keyboard reports. Strategy:

- **ASCII characters** (a-z, 0-9, symbols): Send as individual keystrokes using standard HID keyboard usage codes
- **Chinese characters**: Send using Unicode input method — hold Left GUI + numeric pad sequence encoding the character's Unicode value (Windows/Linux compatible). Note: this requires the target PC to have Unicode input enabled (Win: registry key, Linux: IBus). Alternative: send via clipboard copy (Ctrl+V) if input method is unavailable, but this requires a pre-shared clipboard buffer which is out of scope.
- **Spaces, punctuation**: Map to corresponding HID usage codes
- Send each character with a 20ms press+release interval to avoid overrun
- If active PC is USB device (PC3), route through `usb_device_send_keyboard()`
- If active PC is BLE (PC1/PC2), route through BLE HID

### Memory Management

- I2S DMA buffer: 2× 1024 samples (2KB each) = 4KB
- PCM read buffer: 4KB per read cycle
- WebSocket send: streaming, no large accumulation needed
- ASR result buffer: 256 bytes (sufficient for recognized text)
- Total additional RAM: ~8KB (well within ESP32-S3 capability)

### WiFi Requirement

Voice input requires WiFi STA mode connected to internet. Checks before starting:
1. `voice_asr_enabled` must be true
2. WiFi STA must be connected (`wifi_manager_is_sta_connected()`)
3. If either condition fails: TFT shows brief message, no recording starts

### Error Handling

- **WebSocket connection failure**: Abort recording, TFT shows "ASR connection failed"
- **No internet / DNS failure**: Abort, TFT shows "Network error"
- **ASR returns empty result**: No keyboard input sent (no error shown, just no output)
- **Recording exceeds memory**: I2S DMA handles overflow; streaming sends data continuously so no accumulation
- **Button released before minimum audio**: If recording <200ms, discard (too short for ASR)

---

## Configuration (NVS)

### New fields in `kvm_config_t`

```c
bool voice_asr_enabled;           /* default: false */
char voice_asr_api_key[65];       /* Baidu API Key */
char voice_asr_secret[65];        /* Baidu Secret Key */
char voice_lang[8];               /* "zh" or "en", default: "zh" */
```

### NVS keys (namespace: `kvm_config`)

- `voice_en` — uint8 (0/1 for enabled)
- `voice_ak` — string (API Key)
- `voice_sk` — string (Secret Key, not readable via web API)
- `voice_lang` — string (language code)

### Web API Changes

**GET /api/settings** — add:
- `voice_asr_enabled`: bool
- `voice_asr_api_key`: string (masked if set, e.g. "****abcd")
- `voice_lang`: string

**PATCH /api/settings** — add:
- `voice_asr_enabled`: bool — toggle voice input
- `voice_asr_api_key`: string — set API Key (full value sent, stored securely)
- `voice_asr_secret`: string — set Secret Key (not returned in GET)
- `voice_lang`: string — set language

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
#define HAS_VOICE_INPUT 1
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_I2S_CLK     GPIO_NUM_0    /* I2S clock pin for SPM1423 */
#define MIC_I2S_DATA    GPIO_NUM_34   /* I2S data pin for SPM1423 */
#else
#define HAS_VOICE_INPUT 0
#endif
```

Note: SPM1423 on M5StickS3 uses I2S (not I2C). The I2S pins are separate from the I2C bus used by IMU and PMIC. I2S CLK=GPIO0, I2S DATA=GPIO34. These must be verified against M5StickS3 schematic.

---

## Factory Reset via Web (all boards)

New `POST /api/factory-reset` endpoint available on all boards. The web UI settings page shows a "Factory Reset" button. Clicking it shows a confirmation dialog. If confirmed, sends `{ "confirm": true }`. The handler erases NVS and calls `esp_restart()`.

This is the only factory reset method on boards without a secondary button (StampS3, DevKitC).

---

## Files Summary

| Action | File | Purpose |
|--------|------|---------|
| Create | `mic_driver.h/c` | SPM1423 I2S microphone driver |
| Create | `voice_input.h/c` | Voice input manager (ASR WebSocket, text→keyboard) |
| Modify | `switch_manager.c` | Button behavior: primary→voice, secondary→2-stage reset |
| Modify | `config_manager.h/c` | Add voice ASR config fields, NVS load/save |
| Modify | `web_server.c` | Add voice config to settings, voice_recording to status, factory-reset endpoint |
| Modify | `board.h` | Add HAS_VOICE_INPUT, MIC I2S pin definitions |
| Modify | `tft_display.c` | Recording indicator, factory reset warning overlay |
| Modify | `main.c` | Init mic_driver and voice_input if enabled |
| Modify | `CMakeLists.txt` | Add mic_driver.c, voice_input.c (conditional) |
| Modify | `indicator.h/c` | Add IND_VOICE_RECORDING, IND_FACTORY_WARN states |

---

## Out of Scope

- Local/on-device ASR (ESP32-S3 insufficient compute)
- Voice wake word detection
- Audio playback / speaker output
- Noise cancellation or audio preprocessing beyond basic I2S capture
- Multi-language mixed recognition (single language per session)
- USB microphone input