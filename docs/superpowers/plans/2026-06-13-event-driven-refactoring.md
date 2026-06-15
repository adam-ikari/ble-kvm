# Event-Driven Architecture Refactoring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all direct cross-module function calls with an `esp_event`-based event bus, eliminating tight coupling between modules.

**Architecture:** A single `esp_event_loop` (`app_event_loop`) serves as the system-wide event bus. Each module self-registers event subscriptions in its `_init()` and publishes events when its state changes. HID data flows through the bus synchronously (same task context, zero extra latency).

**Tech Stack:** ESP-IDF v5.5, `esp_event` library (ROM-resident, zero flash cost), NimBLE BLE stack

**Spec:** `docs/superpowers/specs/2026-06-13-event-driven-refactoring-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `main/event_bus.h` | **CREATE** | Event base, event ID enum, all payload structs, convenience macros |
| `main/event_bus.c` | **CREATE** | `event_bus_init()` — creates `app_event_loop` |
| `main/config_manager.h` | MODIFY | Remove `config_save_*()`; add `config_update_*()` and `config_field_t`; keep `config_get_mutable()` for blob RMW |
| `main/config_manager.c` | MODIFY | Implement `config_update_*()` with NVS persistence + `CONFIG_CHANGED` event |
| `main/tft_display.h` | MODIFY | Add `tft_display_toggle_page()` and `tft_display_get_i2c_bus()` declarations |
| `main/ble_peripheral.h` | MODIFY | Remove `ble_peripheral_register_conn_cb()` and callback typedef |
| `main/ble_peripheral.c` | MODIFY | Post `PC_CONNECTED`/`PC_DISCONNECTED` events; subscribe to HID forward events |
| `main/ble_central.h` | MODIFY | Remove unused declarations (none needed — query functions stay) |
| `main/ble_central.c` | MODIFY | Post `KB_CONNECTED`/`MS_CONNECTED`/HID data events instead of direct calls |
| `main/usb_host.c` | MODIFY | Post HID data events instead of calling hid_router directly |
| `main/usb_device.c` | MODIFY | Post `USB_DEVICE_CONNECTED`/`USB_DEVICE_DISCONNECTED` events |
| `main/hid_router.h` | MODIFY | Remove `hid_router_register_activity_cb()` and callback typedef |
| `main/hid_router.c` | MODIFY | Subscribe to HID data events; post forward events + `HID_ACTIVITY` |
| `main/switch_manager.h` | MODIFY | Remove `switch_manager_on_pc_connected/disconnected()` |
| `main/switch_manager.c` | MODIFY | Post events instead of 30+ direct calls; subscribe to connection events |
| `main/anti_idle.h` | MODIFY | Remove `anti_idle_on_activity()`, `anti_idle_on_pc_connected()`, `anti_idle_set_*()` |
| `main/anti_idle.c` | MODIFY | Subscribe to `HID_ACTIVITY`, `PC_CONNECTED`, `CONFIG_CHANGED`; post nudge events |
| `main/power_manager.h` | MODIFY | Remove `pm_sleep_on_activity/pc_connected/pc_disconnected/imu_motion()` |
| `main/power_manager.c` | MODIFY | Subscribe to activity/connection/IMU events; post sleep state events |
| `main/input_mode.c` | MODIFY | Post `INPUT_MODE_CHANGED`, `INPUT_AIR_MOUSE`, `INPUT_IMU_MOTION` |
| `main/voice_input.c` | MODIFY | Subscribe to `VOICE_START_REQUEST`/`VOICE_STOP_REQUEST`; post `VOICE_STATE_CHANGED` |
| `main/wifi_manager.h` | MODIFY | Remove `wifi_manager_register_ready_cb()` (keep internal, or replace with event) |
| `main/wifi_manager.c` | MODIFY | Post `WIFI_MODE_CHANGED`, `WIFI_STA_CONNECTED/DISCONNECTED` |
| `main/web_server.h` | MODIFY | Remove `web_server_notify_switch/connection/device()`, `web_server_grant_auth()` |
| `main/web_server.c` | MODIFY | Subscribe to events for SSE; post events from HTTP handlers |
| `main/main.c` | MODIFY | Add `event_bus_init()`; remove all callback wiring; simplify init sequence |

---

### Task 1: Create `event_bus.h` — Event Definitions

**Files:**
- Create: `main/event_bus.h`

- [ ] **Step 1: Write the header file**

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_event.h"
#include "config_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event Base ─────────────────────────────────────────────────────── */

ESP_EVENT_DECLARE_BASE(APP_EVENT);

/* ── Event IDs ──────────────────────────────────────────────────────── */

typedef enum {
    /* Connection events */
    APP_EVENT_PC_CONNECTED,
    APP_EVENT_PC_DISCONNECTED,
    APP_EVENT_USB_DEVICE_CONNECTED,
    APP_EVENT_USB_DEVICE_DISCONNECTED,
    APP_EVENT_KB_CONNECTED,
    APP_EVENT_KB_DISCONNECTED,
    APP_EVENT_MS_CONNECTED,
    APP_EVENT_MS_DISCONNECTED,

    /* HID data events */
    APP_EVENT_HID_KEYBOARD_DATA,
    APP_EVENT_HID_MOUSE_DATA,
    APP_EVENT_HID_FORWARD_KEYBOARD,
    APP_EVENT_HID_FORWARD_MOUSE,
    APP_EVENT_HID_ACTIVITY,
    APP_EVENT_HID_ANTI_IDLE_NUDGE,
    APP_EVENT_HID_CONSUMER_KEY,

    /* Input events */
    APP_EVENT_INPUT_MODE_CHANGED,
    APP_EVENT_INPUT_AIR_MOUSE,
    APP_EVENT_INPUT_IMU_MOTION,

    /* System/config events */
    APP_EVENT_CONFIG_CHANGED,
    APP_EVENT_PC_SWITCHED,
    APP_EVENT_FACTORY_RESET,
    APP_EVENT_WEB_AUTH_GRANTED,
    APP_EVENT_PAIRING_START,
    APP_EVENT_PAIRING_STOP,
    APP_EVENT_VOICE_START_REQUEST,
    APP_EVENT_VOICE_STOP_REQUEST,
    APP_EVENT_VOICE_STATE_CHANGED,
    APP_EVENT_WIFI_MODE_CHANGED,
    APP_EVENT_WIFI_STA_CONNECTED,
    APP_EVENT_WIFI_STA_DISCONNECTED,
    APP_EVENT_WIFI_AP_READY,
    APP_EVENT_SLEEP_STATE_CHANGED,
    APP_EVENT_SCR_OFF_STATE_CHANGED,
    APP_EVENT_SCAN_START_REQUEST,
} app_event_id_t;

/* ── Event Payload Structs ──────────────────────────────────────────── */

/* Connection events */
typedef struct {
    uint8_t pc_id;
    uint16_t conn_handle;
} app_evt_pc_connected_t;

typedef struct {
    uint8_t pc_id;
} app_evt_pc_disconnected_t;

typedef struct {
    uint16_t conn_handle;
} app_evt_device_connected_t;

/* HID data events — pointer to original buffer, no copy */
typedef struct {
    const uint8_t *data;
    uint8_t len;
} app_evt_hid_data_t;

typedef struct {
    uint8_t pc_id;
} app_evt_anti_idle_nudge_t;

typedef struct {
    uint16_t conn_handle;
    uint16_t usage_code;
} app_evt_consumer_key_t;

/* Input events */
typedef struct {
    uint8_t old_mode;
    uint8_t new_mode;
} app_evt_input_mode_changed_t;

typedef struct {
    int8_t x;
    int8_t y;
} app_evt_air_mouse_t;

/* Config events */
typedef enum {
    CONFIG_FIELD_ACTIVE_PC,
    CONFIG_FIELD_WIFI_SSID,
    CONFIG_FIELD_WIFI_PASSWORD,
    CONFIG_FIELD_WIFI_ENABLED,
    CONFIG_FIELD_WIFI_MODE,
    CONFIG_FIELD_ANTI_IDLE_ENABLED,
    CONFIG_FIELD_ANTI_IDLE_INTERVAL,
    CONFIG_FIELD_INPUT_MODE,
    CONFIG_FIELD_AIR_MOUSE_SENSITIVITY,
    CONFIG_FIELD_USB_MODE,
    CONFIG_FIELD_SCREEN_OFF_TIMEOUT,
    CONFIG_FIELD_SLEEP_TIMEOUT,
    CONFIG_FIELD_DEVICE_NAME,
    CONFIG_FIELD_VOICE_ASR_ENABLED,
    CONFIG_FIELD_VOICE_ASR_APPID,
    CONFIG_FIELD_VOICE_ASR_API_KEY,
    CONFIG_FIELD_VOICE_LANG,
    CONFIG_FIELD_VOICE_INPUT_MODE,
    CONFIG_FIELD_PC_NAMES,
    CONFIG_FIELD_KEYBOARD_MAC,
    CONFIG_FIELD_MOUSE_MAC,
    CONFIG_FIELD_KEYBOARD_NAME,
    CONFIG_FIELD_MOUSE_NAME,
    CONFIG_FIELD_AUTH_TOKEN,
} config_field_t;

typedef struct {
    config_field_t field;
} app_evt_config_changed_t;

/* PC switch event */
typedef struct {
    uint8_t old_pc;
    uint8_t new_pc;
} app_evt_pc_switched_t;

/* WiFi events */
typedef struct {
    uint8_t mode;  /* wifi_operating_mode_t */
} app_evt_wifi_mode_changed_t;

typedef struct {
    char ip[16];
} app_evt_wifi_sta_connected_t;

/* Sleep events */
typedef struct {
    uint8_t state;  /* pm_sleep_state_t */
} app_evt_sleep_state_changed_t;

typedef struct {
    bool off;
} app_evt_scr_off_state_changed_t;

/* Voice events */
typedef struct {
    bool active;
} app_evt_voice_state_changed_t;

/* ── Convenience Macros ─────────────────────────────────────────────── */

/** Post an event to the app event loop. */
#define APP_EVENT_POST(event_id, data, data_size) \
    esp_event_post_to(app_event_loop_handle(), APP_EVENT, (event_id), (data), (data_size), 0)

/** Subscribe to an event on the app event loop. */
#define APP_EVENT_SUBSCRIBE(event_id, handler, handler_arg) \
    esp_event_handler_register_with(app_event_loop_handle(), APP_EVENT, (event_id), (handler), (handler_arg))

/* ── Init ───────────────────────────────────────────────────────────── */

/**
 * Create the application event loop. Must be called once, early in app_main(),
 * before any module that uses APP_EVENT_POST or APP_EVENT_SUBSCRIBE.
 */
void event_bus_init(void);

/**
 * Return the handle to the application event loop.
 * Used internally by the APP_EVENT_POST / APP_EVENT_SUBSCRIBE macros.
 */
esp_event_loop_handle_t app_event_loop_handle(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Commit**

```bash
git add main/event_bus.h
git commit -m "feat: add event_bus.h — event definitions for system-wide event bus"
```

---

### Task 2: Create `event_bus.c` — Event Loop Initialization

**Files:**
- Create: `main/event_bus.c`

- [ ] **Step 1: Write the implementation**

```c
#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "event_bus";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static esp_event_loop_handle_t app_loop = NULL;

void event_bus_init(void)
{
    esp_event_loop_args_t loop_args = {
        .queue_size = 32,
        .task_name = "app_evt",
        .task_priority = 5,
        .task_stack_size = 4096,
        .task_core_id = 0,
    };

    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &app_loop));
    ESP_LOGI(TAG, "Application event loop created");
}

esp_event_loop_handle_t app_event_loop_handle(void)
{
    return app_loop;
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

Modify `main/CMakeLists.txt` — add `event_bus.c` to the `SRCS` list.

- [ ] **Step 3: Commit**

```bash
git add main/event_bus.c main/CMakeLists.txt
git commit -m "feat: add event_bus.c — application event loop initialization"
```

---

### Task 3: Fix `tft_display.h` — Add Missing Declarations

**Files:**
- Modify: `main/tft_display.h`

- [ ] **Step 1: Add the two missing function declarations**

In `main/tft_display.h`, after the existing `tft_display_freeze()` declaration, add:

```c
#include "driver/i2c_master.h"
void tft_display_toggle_page(void);
i2c_master_bus_handle_t tft_display_get_i2c_bus(void);
```

The `#include "driver/i2c_master.h"` is needed for the `i2c_master_bus_handle_t` type.

- [ ] **Step 2: Commit**

```bash
git add main/tft_display.h
git commit -m "refactor: add tft_display_toggle_page and tft_display_get_i2c_bus to header"
```

---

### Task 4: Refactor `config_manager` — Remove `config_save_*()`, Add `config_update_*()`

**Files:**
- Modify: `main/config_manager.h`
- Modify: `main/config_manager.c`

- [ ] **Step 1: Update `config_manager.h`**

Remove the public `config_save_*()` declarations but keep `config_get_mutable()` as an internal escape hatch for blob updates:
```c
// REMOVE all of these:
void config_save_pcs(void);
void config_save_input_devices(void);
void config_save_active_pc(void);
void config_save_auth_token(void);
void config_save_wifi(void);
void config_save_anti_idle(void);
void config_save_input_mode(void);
void config_save_usb_mode(void);
void config_save_voice(void);
void config_save_sleep(void);
void config_save_device_name(void);

// KEEP config_get_mutable() — needed ONLY for read-modify-write on blob structs
// (e.g., modify one field of kvm_pc_config_t then call config_update_blob)
// Direct mutation of scalar fields is forbidden — use config_update_*() instead.
kvm_config_t *config_get_mutable(void);
```

Add:
```c
/* config_field_t enum is defined in event_bus.h */

void config_update_u8(config_field_t field, uint8_t value);
void config_update_u16(config_field_t field, uint16_t value);
void config_update_u32(config_field_t field, uint32_t value);
void config_update_bool(config_field_t field, bool value);
void config_update_str(config_field_t field, const char *value);
void config_update_blob(config_field_t field, const void *data, size_t len);
```

Keep:
```c
void config_manager_init(void);
const kvm_config_t *config_get(void);
void config_generate_auth_token(void);
void config_manager_deinit(void);
```

- [ ] **Step 2: Update `config_manager.c`**

Remove all individual `config_save_*()` function bodies (they become static helper calls inside `config_update_*()`).

Add the `config_update_*()` implementations. Each function:
1. Validates the value
2. Updates the in-memory `config` struct
3. Saves to NVS
4. Posts `APP_EVENT_CONFIG_CHANGED`

```c
#include "event_bus.h"

void config_update_u8(config_field_t field, uint8_t value)
{
    switch (field) {
    case CONFIG_FIELD_ACTIVE_PC:
        if (value < 1 || value > 3) return;
        config.active_pc = value;
        nvs_set_u8(nvs_config, "active_pc", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_INPUT_MODE:
        if (value > 1) return;
        config.input_mode = value;
        nvs_set_u8(nvs_config, "input_mode", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_USB_MODE:
        if (value > USB_MODE_HOST) return;
        config.usb_mode = value;
        nvs_set_u8(nvs_config, "usb_mode", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_VOICE_INPUT_MODE:
        if (value > 2) return;
        config.voice_input_mode = value;
        nvs_set_u8(nvs_config, "voice_im", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_AIR_MOUSE_SENSITIVITY:
        if (value < 1 || value > 10) return;
        config.air_mouse_sensitivity = value;
        nvs_set_u8(nvs_config, "air_sens", value);
        nvs_commit(nvs_config);
        break;
    default:
        ESP_LOGW(TAG, "config_update_u8: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

void config_update_u16(config_field_t field, uint16_t value)
{
    switch (field) {
    case CONFIG_FIELD_ANTI_IDLE_INTERVAL:
        if (value < 10) value = 10;
        if (value > 3600) value = 3600;
        config.anti_idle_interval_sec = value;
        nvs_set_u16(nvs_config, "anti_idle_ivl", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_SCREEN_OFF_TIMEOUT:
        config.screen_off_timeout_sec = value;
        nvs_set_u16(nvs_config, "scr_off_to", value);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_SLEEP_TIMEOUT:
        config.sleep_timeout_sec = value;
        nvs_set_u16(nvs_config, "sleep_to", value);
        nvs_commit(nvs_config);
        break;
    default:
        ESP_LOGW(TAG, "config_update_u16: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

void config_update_u32(config_field_t field, uint32_t value)
{
    switch (field) {
    case CONFIG_FIELD_VOICE_ASR_APPID:
        config.voice_asr_appid = value;
        nvs_set_u32(nvs_config, "voice_appid", value);
        nvs_commit(nvs_config);
        break;
    default:
        ESP_LOGW(TAG, "config_update_u32: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

void config_update_bool(config_field_t field, bool value)
{
    uint8_t v = value ? 1 : 0;

    switch (field) {
    case CONFIG_FIELD_ANTI_IDLE_ENABLED:
        config.anti_idle_enabled = value;
        nvs_set_u8(nvs_config, "anti_idle", v);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_WIFI_ENABLED:
        config.wifi_enabled = value;
        nvs_set_u8(nvs_wifi, "enabled", v);
        nvs_commit(nvs_wifi);
        break;
    case CONFIG_FIELD_VOICE_ASR_ENABLED:
        config.voice_asr_enabled = value;
        nvs_set_u8(nvs_config, "voice_en", v);
        nvs_commit(nvs_config);
        break;
    default:
        ESP_LOGW(TAG, "config_update_bool: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

void config_update_str(config_field_t field, const char *value)
{
    if (!value) return;

    switch (field) {
    case CONFIG_FIELD_WIFI_SSID:
        strncpy(config.wifi_ssid, value, sizeof(config.wifi_ssid) - 1);
        config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
        nvs_set_str(nvs_wifi, "ssid", config.wifi_ssid);
        nvs_commit(nvs_wifi);
        break;
    case CONFIG_FIELD_WIFI_PASSWORD:
        strncpy(config.wifi_password, value, sizeof(config.wifi_password) - 1);
        config.wifi_password[sizeof(config.wifi_password) - 1] = '\0';
        nvs_set_str(nvs_wifi, "password", config.wifi_password);
        nvs_commit(nvs_wifi);
        break;
    case CONFIG_FIELD_DEVICE_NAME:
        strncpy(config.device_name, value, DEVICE_NAME_MAX - 1);
        config.device_name[DEVICE_NAME_MAX - 1] = '\0';
        nvs_set_str(nvs_config, "dev_name", config.device_name);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_VOICE_ASR_API_KEY:
        strncpy(config.voice_asr_api_key, value, 64);
        config.voice_asr_api_key[64] = '\0';
        nvs_set_str(nvs_config, "voice_ak", config.voice_asr_api_key);
        nvs_commit(nvs_config);
        break;
    case CONFIG_FIELD_VOICE_LANG:
        strncpy(config.voice_lang, value, sizeof(config.voice_lang) - 1);
        config.voice_lang[sizeof(config.voice_lang) - 1] = '\0';
        nvs_set_str(nvs_config, "voice_lang", config.voice_lang);
        nvs_commit(nvs_config);
        break;
    default:
        ESP_LOGW(TAG, "config_update_str: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

void config_update_blob(config_field_t field, const void *data, size_t len)
{
    switch (field) {
    case CONFIG_FIELD_PC_NAMES:
        if (len != sizeof(config.pcs)) return;
        memcpy(config.pcs, data, len);
        nvs_set_blob(nvs_ble, "pcs", config.pcs, sizeof(config.pcs));
        nvs_commit(nvs_ble);
        break;
    case CONFIG_FIELD_KEYBOARD_MAC:
        if (len != sizeof(input_device_t)) return;
        memcpy(&config.keyboard, data, len);
        nvs_set_blob(nvs_ble, "keyboard", &config.keyboard, sizeof(config.keyboard));
        nvs_commit(nvs_ble);
        break;
    case CONFIG_FIELD_MOUSE_MAC:
        if (len != sizeof(input_device_t)) return;
        memcpy(&config.mouse, data, len);
        nvs_set_blob(nvs_ble, "mouse", &config.mouse, sizeof(config.mouse));
        nvs_commit(nvs_ble);
        break;
    default:
        ESP_LOGW(TAG, "config_update_blob: unsupported field %d", field);
        return;
    }

    app_evt_config_changed_t evt = { .field = field };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}
```

Also make `config_generate_auth_token()` post `CONFIG_CHANGED` after regenerating:
```c
void config_generate_auth_token(void)
{
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < AUTH_TOKEN_LEN - 1; i++) {
        config.auth_token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    config.auth_token[AUTH_TOKEN_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Generated auth token: %s", config.auth_token);
    nvs_set_str(nvs_config, "auth_token", config.auth_token);
    nvs_commit(nvs_config);

    app_evt_config_changed_t evt = { .field = CONFIG_FIELD_AUTH_TOKEN };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds. Warnings for unused `config_save_*` functions may appear (they'll be removed when callers are converted in later tasks).

- [ ] **Step 4: Commit**

```bash
git add main/config_manager.h main/config_manager.c
git commit -m "refactor: replace config_get_mutable with config_update_* event-driven API"
```

---

### Task 5: Refactor `ble_peripheral` — Post Connection Events

**Files:**
- Modify: `main/ble_peripheral.h`
- Modify: `main/ble_peripheral.c`

- [ ] **Step 1: Update `ble_peripheral.h`**

Remove:
```c
typedef void (*ble_peripheral_conn_cb_t)(uint8_t pc_id, uint16_t conn_handle, bool connected);
void ble_peripheral_register_conn_cb(ble_peripheral_conn_cb_t cb);
```

Add `#include "event_bus.h"` at the top.

- [ ] **Step 2: Update `ble_peripheral.c`**

Add `#include "event_bus.h"`.

Remove the static `conn_cb` variable and `ble_peripheral_register_conn_cb()` function.

In `ble_gap_event_handler()`, replace the `conn_cb` calls with event posts:

**BLE_GAP_EVENT_CONNECT (success path, line ~367):**
```c
/* Replace:
    if (conn_cb) {
        conn_cb(slot + 1, event->connect.conn_handle, true);
    }
*/
app_evt_pc_connected_t evt = {
    .pc_id = slot + 1,
    .conn_handle = event->connect.conn_handle,
};
APP_EVENT_POST(APP_EVENT_PC_CONNECTED, &evt, sizeof(evt));
```

**BLE_GAP_EVENT_DISCONNECT (line ~396):**
```c
/* Replace:
    if (conn_cb) {
        conn_cb(slot + 1, 0, false);
    }
*/
app_evt_pc_disconnected_t evt = { .pc_id = slot + 1 };
APP_EVENT_POST(APP_EVENT_PC_DISCONNECTED, &evt, sizeof(evt));
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds. Warnings about unused `conn_cb` may appear (already removed).

- [ ] **Step 4: Commit**

```bash
git add main/ble_peripheral.h main/ble_peripheral.c
git commit -m "refactor: ble_peripheral posts PC_CONNECTED/PC_DISCONNECTED events instead of callback"
```

---

### Task 6: Refactor `ble_central` — Post Device Connection + HID Data Events

**Files:**
- Modify: `main/ble_central.c`

- [ ] **Step 1: Update `ble_central.c`**

Add `#include "event_bus.h"`.

In `ble_central_gap_event()`, after keyboard connection confirmed (line ~278):
```c
/* After setting keyboard_conn_handle and logging */
app_evt_device_connected_t evt = { .conn_handle = event->connect.conn_handle };
APP_EVENT_POST(APP_EVENT_KB_CONNECTED, &evt, sizeof(evt));
```

After mouse connection confirmed (line ~278, else branch):
```c
app_evt_device_connected_t evt = { .conn_handle = event->connect.conn_handle };
APP_EVENT_POST(APP_EVENT_MS_CONNECTED, &evt, sizeof(evt));
```

In `BLE_GAP_EVENT_DISCONNECT`, after keyboard disconnect (line ~324):
```c
APP_EVENT_POST(APP_EVENT_KB_DISCONNECTED, NULL, 0);
```

After mouse disconnect (line ~339):
```c
APP_EVENT_POST(APP_EVENT_MS_DISCONNECTED, NULL, 0);
```

In `BLE_GAP_EVENT_NOTIFY_RX` (line ~357), replace direct `hid_router_forward_*()` calls:
```c
/* Replace:
    if (ch == keyboard_conn_handle) {
        hid_router_forward_keyboard(data, data_len);
    } else if (ch == mouse_conn_handle) {
        hid_router_forward_mouse(data, data_len);
    }
*/
app_evt_hid_data_t hid_evt = {
    .data = data,
    .len = (uint8_t)data_len,
};
if (ch == keyboard_conn_handle) {
    APP_EVENT_POST(APP_EVENT_HID_KEYBOARD_DATA, &hid_evt, sizeof(hid_evt));
} else if (ch == mouse_conn_handle) {
    APP_EVENT_POST(APP_EVENT_HID_MOUSE_DATA, &hid_evt, sizeof(hid_evt));
}
```

Remove `#include "hid_router.h"` — no longer needed.

- [ ] **Step 2: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add main/ble_central.c
git commit -m "refactor: ble_central posts KB/MS connection and HID data events"
```

---

### Task 6b: Refactor `usb_host.c` and `usb_device.c` — Post Events

**Files:**
- Modify: `main/usb_host.c`
- Modify: `main/usb_device.c`

- [ ] **Step 1: Update `usb_host.c`**

Add `#include "event_bus.h"`.
Remove `#include "hid_router.h"`.

Replace direct `hid_router_forward_*()` calls in the USB HID callback functions:

In USB keyboard report handler:
```c
/* Replace: hid_router_forward_keyboard(data, len); */
app_evt_hid_data_t hid_evt = { .data = data, .len = len };
APP_EVENT_POST(APP_EVENT_HID_KEYBOARD_DATA, &hid_evt, sizeof(hid_evt));
```

In USB mouse report handler:
```c
/* Replace: hid_router_forward_mouse(data, len); */
app_evt_hid_data_t hid_evt = { .data = data, .len = len };
APP_EVENT_POST(APP_EVENT_HID_MOUSE_DATA, &hid_evt, sizeof(hid_evt));
```

- [ ] **Step 2: Update `usb_device.c`**

Add `#include "event_bus.h"`.

Post events in USB device connect/disconnect:
```c
/* In USB device connect callback: */
APP_EVENT_POST(APP_EVENT_USB_DEVICE_CONNECTED, NULL, 0);

/* In USB device disconnect callback: */
APP_EVENT_POST(APP_EVENT_USB_DEVICE_DISCONNECTED, NULL, 0);
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/usb_host.c main/usb_device.c
git commit -m "refactor: USB host posts HID data events, USB device posts connection events"
```

---

### Task 7: Refactor `hid_router` — Subscribe to HID Data, Post Forward Events

**Files:**
- Modify: `main/hid_router.h`
- Modify: `main/hid_router.c`

- [ ] **Step 1: Update `hid_router.h`**

Remove:
```c
typedef void (*hid_activity_cb_t)(void);
void hid_router_register_activity_cb(hid_activity_cb_t cb);
```

- [ ] **Step 2: Update `hid_router.c`**

Add `#include "event_bus.h"`.

Remove `activity_cb` static variable and `hid_router_register_activity_cb()`.

Add event handler:
```c
static void hid_keyboard_data_handler(void *arg, esp_event_base_t base,
                                       int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    const kvm_config_t *cfg = config_get();

    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        int rc = usb_device_send_keyboard(evt->data, evt->len);
        if (rc != 0) {
            ESP_LOGW(TAG, "USB keyboard forward failed: rc=%d", rc);
        }
#endif
    } else {
        uint16_t conn_handle = switch_manager_get_active_conn_handle();
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
        int rc = ble_peripheral_send_hid_report(conn_handle, 1, evt->data, evt->len);
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE keyboard forward failed: rc=%d", rc);
        }
    }

    APP_EVENT_POST(APP_EVENT_HID_ACTIVITY, NULL, 0);
}

static void hid_mouse_data_handler(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    const kvm_config_t *cfg = config_get();

    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        int rc = usb_device_send_mouse(evt->data, evt->len);
        if (rc != 0) {
            ESP_LOGW(TAG, "USB mouse forward failed: rc=%d", rc);
        }
#endif
    } else {
        uint16_t conn_handle = switch_manager_get_active_conn_handle();
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
        int rc = ble_peripheral_send_hid_report(conn_handle, 2, evt->data, evt->len);
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE mouse forward failed: rc=%d", rc);
        }
    }

    APP_EVENT_POST(APP_EVENT_HID_ACTIVITY, NULL, 0);
}
```

Update `hid_router_init()`:
```c
void hid_router_init(void)
{
    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_KEYBOARD_DATA, hid_keyboard_data_handler, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_MOUSE_DATA, hid_mouse_data_handler, NULL);
    ESP_LOGI(TAG, "HID router initialized");
}
```

Remove the old `hid_router_forward_keyboard()`, `hid_router_forward_mouse()` bodies (or keep them as static helpers called by the event handlers).

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/hid_router.h main/hid_router.c
git commit -m "refactor: hid_router subscribes to HID data events, posts HID_ACTIVITY"
```

---

### Task 8: Refactor `switch_manager` — Post Events Instead of Direct Calls

**Files:**
- Modify: `main/switch_manager.h`
- Modify: `main/switch_manager.c`

- [ ] **Step 1: Update `switch_manager.h`**

Remove:
```c
void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle);
void switch_manager_on_pc_disconnected(uint8_t pc_id);
```

- [ ] **Step 2: Update `switch_manager.c`**

Add `#include "event_bus.h"`.

Remove `#include "anti_idle.h"`, `#include "web_server.h"`, `#include "voice_input.h"`, `#include "power_manager.h"`, `#include "input_mode.h"` — these are now handled via events.

Remove `extern void web_server_grant_auth(void)` and `extern void tft_display_toggle_page(void)` — replace with `#include "tft_display.h"` and event post.

Add event subscription handlers:
```c
static void on_pc_connected(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    app_evt_pc_connected_t *evt = (app_evt_pc_connected_t *)event_data;
    ESP_LOGI(TAG, "PC%d connected (handle=%d)", evt->pc_id, evt->conn_handle);
    update_led_state();
}

static void on_pc_disconnected(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    app_evt_pc_disconnected_t *evt = (app_evt_pc_disconnected_t *)event_data;
    ESP_LOGI(TAG, "PC%d disconnected", evt->pc_id);

    /* Auto-switch active PC if the disconnected PC was active */
    const kvm_config_t *cfg = config_get();
    if (cfg->active_pc == evt->pc_id) {
        for (uint8_t candidate = 1; candidate <= 3; candidate++) {
            if (candidate == evt->pc_id) continue;
            if (candidate == 3) {
                if (is_pc3_connected()) {
                    config_update_u8(CONFIG_FIELD_ACTIVE_PC, candidate);
                    break;
                }
            } else {
                if (ble_peripheral_is_pc_connected(candidate - 1)) {
                    config_update_u8(CONFIG_FIELD_ACTIVE_PC, candidate);
                    break;
                }
            }
        }
    }
    update_led_state();
}

static void on_usb_device_connected(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    update_led_state();
}

static void on_usb_device_disconnected(void *arg, esp_event_base_t base,
                                        int32_t event_id, void *event_data)
{
    update_led_state();
}
```

In `switch_task_func()`, replace the CMD handlers:

**CMD_SWITCH (line ~114):**
```c
if (cmd == CMD_SWITCH) {
    if (input_mode_get() != INPUT_MODE_KVM) {
        input_mode_on_primary_button();
    } else {
        const kvm_config_t *cfg = config_get();
        uint8_t old_pc = cfg->active_pc;
        uint8_t new_pc = old_pc;

        for (int attempt = 0; attempt < 3; attempt++) {
            new_pc = (new_pc % 3) + 1;
            if (new_pc == 3) {
                if (is_pc3_connected()) break;
            } else {
                if (ble_peripheral_is_pc_connected(new_pc - 1)) break;
            }
        }
        if (new_pc == old_pc) {
            ESP_LOGW(TAG, "No other connected PC to switch to");
            update_led_state();
            continue;
        }

        indicator_set_state(IND_PAIRING);
        vTaskDelay(pdMS_TO_TICKS(100));

        config_update_u8(CONFIG_FIELD_ACTIVE_PC, new_pc);

        ESP_LOGI(TAG, "Switched from PC%d to PC%d", old_pc, new_pc);
        update_led_state();

        app_evt_pc_switched_t evt = { .old_pc = old_pc, .new_pc = new_pc };
        APP_EVENT_POST(APP_EVENT_PC_SWITCHED, &evt, sizeof(evt));
    }
}
```

**CMD_SECONDARY (line ~150):**
```c
else if (cmd == CMD_SECONDARY) {
    if (input_mode_get() != INPUT_MODE_KVM) {
        input_mode_on_secondary_button();
    } else {
        tft_display_toggle_page();
    }
}
```

**CMD_FACTORY_RST (line ~159):**
```c
else if (cmd == CMD_FACTORY_RST) {
    APP_EVENT_POST(APP_EVENT_FACTORY_RESET, NULL, 0);
}
```

**CMD_WEB_AUTH (line ~189):**
```c
else if (cmd == CMD_WEB_AUTH) {
    APP_EVENT_POST(APP_EVENT_WEB_AUTH_GRANTED, NULL, 0);
    ESP_LOGI(TAG, "Web auth granted via double-click");
}
```

**CMD_VOICE_START (line ~165):**
```c
else if (cmd == CMD_VOICE_START) {
    APP_EVENT_POST(APP_EVENT_VOICE_START_REQUEST, NULL, 0);
}
```

**CMD_VOICE_STOP (line ~174):**
```c
else if (cmd == CMD_VOICE_STOP) {
    APP_EVENT_POST(APP_EVENT_VOICE_STOP_REQUEST, NULL, 0);
}
```

**CMD_PPT_PAGE_UP (line ~194):**
```c
else if (cmd == CMD_PPT_PAGE_UP) {
    uint16_t conn = switch_manager_get_active_conn_handle();
    if (conn != 0xFFFF && conn != 0) {
        app_evt_consumer_key_t evt = { .conn_handle = conn, .usage_code = 0x004B };
        APP_EVENT_POST(APP_EVENT_HID_CONSUMER_KEY, &evt, sizeof(evt));
    }
}
```

**CMD_FACTORY_WARN (line ~181):**
```c
else if (cmd == CMD_FACTORY_WARN) {
    ESP_LOGW(TAG, "Factory reset warning — hold 10s to confirm");
    indicator_set_state(IND_PAIRING);
}
```

**CMD_FACTORY_CANCEL (line ~185):**
```c
else if (cmd == CMD_FACTORY_CANCEL) {
    ESP_LOGI(TAG, "Factory reset cancelled");
    update_led_state();
}
```

Remove `switch_manager_on_pc_connected()` and `switch_manager_on_pc_disconnected()`.

Update `switch_manager_init()` to subscribe to events:
```c
void switch_manager_init(void)
{
    /* ... existing queue, task, GPIO, timer setup ... */

    APP_EVENT_SUBSCRIBE(APP_EVENT_PC_CONNECTED, on_pc_connected, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_PC_DISCONNECTED, on_pc_disconnected, NULL);
#if HAS_USB
    APP_EVENT_SUBSCRIBE(APP_EVENT_USB_DEVICE_CONNECTED, on_usb_device_connected, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_USB_DEVICE_DISCONNECTED, on_usb_device_disconnected, NULL);
#endif

    ESP_LOGI(TAG, "Switch manager initialized");
}
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/switch_manager.h main/switch_manager.c
git commit -m "refactor: switch_manager posts events instead of direct cross-module calls"
```

---

### Task 9: Refactor `anti_idle` — Subscribe to Events

**Files:**
- Modify: `main/anti_idle.h`
- Modify: `main/anti_idle.c`

- [ ] **Step 1: Update `anti_idle.h`**

Remove:
```c
void anti_idle_on_activity(void);
void anti_idle_set_enabled(bool enabled);
void anti_idle_set_interval(uint16_t interval_sec);
void anti_idle_on_pc_connected(uint8_t pc_id);
```

- [ ] **Step 2: Update `anti_idle.c`**

Add `#include "event_bus.h"`.

Remove `#include "switch_manager.h"`, `#include "ble_peripheral.h"`, `#include "ble_central.h"`, `#include "usb_device.h"`, `#include "usb_host.h"`.

Remove the old `anti_idle_on_activity()`, `anti_idle_set_enabled()`, `anti_idle_set_interval()`, `anti_idle_on_pc_connected()` functions.

Add event handlers:
```c
static void on_hid_activity(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    int64_t now = esp_timer_get_time();
    bool is_active;
    portENTER_CRITICAL(&anti_idle_spinlock);
    is_active = active;
    for (int i = 0; i < 3; i++) {
        pc_last_activity[i] = now;
    }
    portEXIT_CRITICAL(&anti_idle_spinlock);
    if (is_active) {
        restart_timer();
    }
}

static void on_pc_connected(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    app_evt_pc_connected_t *evt = (app_evt_pc_connected_t *)event_data;
    if (evt->pc_id < 1 || evt->pc_id > 3) return;
    portENTER_CRITICAL(&anti_idle_spinlock);
    pc_last_activity[evt->pc_id - 1] = esp_timer_get_time();
    portEXIT_CRITICAL(&anti_idle_spinlock);
}

static void on_config_changed(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    app_evt_config_changed_t *evt = (app_evt_config_changed_t *)event_data;
    if (evt->field == CONFIG_FIELD_ANTI_IDLE_ENABLED ||
        evt->field == CONFIG_FIELD_ANTI_IDLE_INTERVAL) {
        restart_timer();
    }
}
```

Update `send_mouse_nudge_to_pc()` to post events instead of calling `ble_peripheral_send_hid_report()` directly:
```c
static void send_mouse_nudge_to_pc(uint8_t pc_id)
{
    const kvm_config_t *cfg = config_get();

    if (pc_id == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        if (!usb_device_is_connected()) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        usb_device_send_mouse(report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        usb_device_send_mouse(report2, sizeof(report2));
#endif
    } else {
        uint16_t conn = ble_peripheral_get_conn_handle(pc_id - 1);
        if (conn == 0 || conn == 0xFFFF) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report2, sizeof(report2));
    }
}
```

Update `anti_idle_init()`:
```c
void anti_idle_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = idle_timer_cb,
        .name = "anti_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &idle_timer));

    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_ACTIVITY, on_hid_activity, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_PC_CONNECTED, on_pc_connected, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_CONFIG_CHANGED, on_config_changed, NULL);

    if (config_get()->anti_idle_enabled) {
        restart_timer();
    }
    ESP_LOGI(TAG, "Anti-idle initialized (enabled=%d, interval=%ds)",
             config_get()->anti_idle_enabled, config_get()->anti_idle_interval_sec);
}
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/anti_idle.h main/anti_idle.c
git commit -m "refactor: anti_idle subscribes to HID_ACTIVITY, PC_CONNECTED, CONFIG_CHANGED events"
```

---

### Task 10: Refactor `power_manager` — Subscribe to Events

**Files:**
- Modify: `main/power_manager.h`
- Modify: `main/power_manager.c`

- [ ] **Step 1: Update `power_manager.h`**

Remove:
```c
void pm_sleep_on_activity(void);
void pm_sleep_on_pc_connected(uint8_t pc_id);
void pm_sleep_on_pc_disconnected(uint8_t pc_id);
void pm_sleep_on_imu_motion(void);
```

- [ ] **Step 2: Update `power_manager.c`**

Add `#include "event_bus.h"`.

Add event handlers:
```c
static void on_hid_activity(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    if (sleep_state == PM_STATE_SCREEN_OFF) {
        exit_screen_off();
    }
    restart_screen_off_timer();
}

static void on_pc_connected(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    portENTER_CRITICAL(&pm_spinlock);
    connected_pc_count++;
    portEXIT_CRITICAL(&pm_spinlock);

    esp_timer_stop(sleep_timer);
    if (sleep_state == PM_STATE_SCREEN_OFF) {
        exit_screen_off();
    }
    restart_screen_off_timer();
}

static void on_pc_disconnected(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    portENTER_CRITICAL(&pm_spinlock);
    if (connected_pc_count > 0) connected_pc_count--;
    portEXIT_CRITICAL(&pm_spinlock);

    if (all_pcs_disconnected()) {
        restart_sleep_timer();
    }
}

static void on_imu_motion(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    if (config_get()->input_mode == 1 /* PPT */) {
        restart_sleep_timer();
    }
}

static void on_config_changed(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    app_evt_config_changed_t *evt = (app_evt_config_changed_t *)event_data;
    if (evt->field == CONFIG_FIELD_SCREEN_OFF_TIMEOUT) {
        restart_screen_off_timer();
    } else if (evt->field == CONFIG_FIELD_SLEEP_TIMEOUT) {
        restart_sleep_timer();
    }
}
```

Remove the old `pm_sleep_on_activity()`, `pm_sleep_on_pc_connected()`, `pm_sleep_on_pc_disconnected()`, `pm_sleep_on_imu_motion()` function bodies.

Update `pm_sleep_init()`:
```c
void pm_sleep_init(void)
{
    /* ... existing timer creation ... */

    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_ACTIVITY, on_hid_activity, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_PC_CONNECTED, on_pc_connected, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_PC_DISCONNECTED, on_pc_disconnected, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_INPUT_IMU_MOTION, on_imu_motion, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_CONFIG_CHANGED, on_config_changed, NULL);

    /* ... existing timer start ... */
}
```

Post sleep state events in `enter_screen_off()`, `exit_screen_off()`, `enter_sleep()`:
```c
static void enter_screen_off(void)
{
    if (sleep_state != PM_STATE_ACTIVE) return;
    sleep_state = PM_STATE_SCREEN_OFF;
    ESP_LOGI(TAG_SLEEP, "Screen off");
    /* ... existing TFT code ... */

    app_evt_scr_off_state_changed_t evt = { .off = true };
    APP_EVENT_POST(APP_EVENT_SCR_OFF_STATE_CHANGED, &evt, sizeof(evt));
}

static void exit_screen_off(void)
{
    if (sleep_state != PM_STATE_SCREEN_OFF) return;
    sleep_state = PM_STATE_ACTIVE;
    ESP_LOGI(TAG_SLEEP, "Screen on");
    /* ... existing TFT code ... */

    app_evt_scr_off_state_changed_t evt = { .off = false };
    APP_EVENT_POST(APP_EVENT_SCR_OFF_STATE_CHANGED, &evt, sizeof(evt));
}

static void enter_sleep(void)
{
    /* ... existing sleep code ... */
    app_evt_sleep_state_changed_t evt = { .state = PM_STATE_SLEEP };
    APP_EVENT_POST(APP_EVENT_SLEEP_STATE_CHANGED, &evt, sizeof(evt));

    /* ... light sleep ... */

    /* Woke up */
    evt.state = PM_STATE_ACTIVE;
    APP_EVENT_POST(APP_EVENT_SLEEP_STATE_CHANGED, &evt, sizeof(evt));
}
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/power_manager.h main/power_manager.c
git commit -m "refactor: power_manager subscribes to activity/connection/IMU events"
```

---

### Task 11: Refactor `input_mode` — Post Mode Change and Air Mouse Events

**Files:**
- Modify: `main/input_mode.c`

- [ ] **Step 1: Update `input_mode.c`**

Add `#include "event_bus.h"`.

In `input_mode_set()`:
```c
void input_mode_set(input_mode_t mode)
{
    if (mode == current_mode) return;
    if (current_mode == INPUT_MODE_PPT_AIR) stop_air_mouse();

    input_mode_t old = current_mode;
    current_mode = mode;
    config_update_u8(CONFIG_FIELD_INPUT_MODE, (uint8_t)mode);

    if (mode == INPUT_MODE_PPT_AIR) start_air_mouse();

    app_evt_input_mode_changed_t evt = { .old_mode = (uint8_t)old, .new_mode = (uint8_t)mode };
    APP_EVENT_POST(APP_EVENT_INPUT_MODE_CHANGED, &evt, sizeof(evt));

    ESP_LOGI(TAG, "Mode set to %d", mode);
}
```

In `air_mouse_task_func()`, after reading cursor:
```c
if (cursor.x != 0 || cursor.y != 0) {
    uint8_t report[4] = {0x00,
                         (uint8_t)(cursor.x & 0xFF),
                         (uint8_t)(cursor.y & 0xFF),
                         0x00};
    uint16_t conn = switch_manager_get_active_conn_handle();
    if (conn) ble_peripheral_send_hid_report(conn, 2, report, 4);

    /* Notify power manager of IMU activity */
    APP_EVENT_POST(APP_EVENT_INPUT_IMU_MOTION, NULL, 0);
}
```

- [ ] **Step 2: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add main/input_mode.c
git commit -m "refactor: input_mode posts INPUT_MODE_CHANGED and INPUT_IMU_MOTION events"
```

---

### Task 12: Refactor `voice_input` — Subscribe to Voice Request Events

**Files:**
- Modify: `main/voice_input.c`

- [ ] **Step 1: Update `voice_input.c`**

Add `#include "event_bus.h"`.

Add event handlers:
```c
static void on_voice_start_request(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    if (!voice_input_is_active()) {
        if (!voice_input_start()) {
            ESP_LOGW(TAG, "Voice start failed (need WiFi/config)");
        }
    }
}

static void on_voice_stop_request(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    if (voice_input_is_active()) {
        voice_input_stop();
    }
}
```

Update `voice_input_init()`:
```c
void voice_input_init(void)
{
    finish_sem = xSemaphoreCreateBinary();
    APP_EVENT_SUBSCRIBE(APP_EVENT_VOICE_START_REQUEST, on_voice_start_request, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_VOICE_STOP_REQUEST, on_voice_stop_request, NULL);
    ESP_LOGI(TAG, "Voice input initialized");
}
```

Post `VOICE_STATE_CHANGED` in `voice_input_start()` (after setting state to VOICE_CONNECTING):
```c
app_evt_voice_state_changed_t evt = { .active = true };
APP_EVENT_POST(APP_EVENT_VOICE_STATE_CHANGED, &evt, sizeof(evt));
```

Post `VOICE_STATE_CHANGED` at the end of `voice_task_func()` (when state returns to VOICE_IDLE):
```c
voice_state = VOICE_IDLE;
app_evt_voice_state_changed_t evt = { .active = false };
APP_EVENT_POST(APP_EVENT_VOICE_STATE_CHANGED, &evt, sizeof(evt));
```

- [ ] **Step 2: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add main/voice_input.c
git commit -m "refactor: voice_input subscribes to voice request events, posts VOICE_STATE_CHANGED"
```

---

### Task 13: Refactor `wifi_manager` — Post WiFi State Events

**Files:**
- Modify: `main/wifi_manager.h`
- Modify: `main/wifi_manager.c`

- [ ] **Step 1: Update `wifi_manager.h`**

Remove:
```c
typedef void (*wifi_ready_cb_t)(void);
void wifi_manager_register_ready_cb(wifi_ready_cb_t cb);
```

- [ ] **Step 2: Update `wifi_manager.c`**

Add `#include "event_bus.h"`.

Remove `ready_cb` static variable and `wifi_manager_register_ready_cb()`.

Also fix `wifi_manager_start_sta()` — replace `config_get_mutable()` + `config_save_wifi()`:
```c
void wifi_manager_start_sta(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    // Save to config via event-driven API
    config_update_str(CONFIG_FIELD_WIFI_SSID, ssid);
    config_update_str(CONFIG_FIELD_WIFI_PASSWORD, password);
    config_update_bool(CONFIG_FIELD_WIFI_ENABLED, true);

    // ... rest unchanged ...
}
```

And fix `wifi_manager_stop_sta()`:
```c
void wifi_manager_stop_sta(void)
{
    // ... existing disconnect logic ...

    // Save to config via event-driven API
    config_update_bool(CONFIG_FIELD_WIFI_ENABLED, false);

    ESP_LOGI(TAG, "STA stopped");
}
```

In `wifi_event_handler()`, replace the `ready_cb` call:
```c
case WIFI_EVENT_AP_START:
    ESP_LOGI(TAG, "AP started, netif ready");
    APP_EVENT_POST(APP_EVENT_WIFI_AP_READY, NULL, 0);
    break;
```

Also post WiFi state events:
```c
case WIFI_EVENT_STA_CONNECTED:
    ESP_LOGI(TAG, "STA: connected to AP");
    break;

/* In IP_EVENT_STA_GOT_IP: */
case IP_EVENT_STA_GOT_IP: {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "STA: got ip %s", sta_ip);
    sta_connected = true;

    app_evt_wifi_sta_connected_t evt;
    strncpy(evt.ip, sta_ip, sizeof(evt.ip) - 1);
    evt.ip[sizeof(evt.ip) - 1] = '\0';
    APP_EVENT_POST(APP_EVENT_WIFI_STA_CONNECTED, &evt, sizeof(evt));
    break;
}

case WIFI_EVENT_STA_DISCONNECTED:
    ESP_LOGI(TAG, "STA: disconnected, reconnecting...");
    sta_connected = false;
    APP_EVENT_POST(APP_EVENT_WIFI_STA_DISCONNECTED, NULL, 0);
    if (current_mode == KVM_WIFI_STA_ONLY || current_mode == KVM_WIFI_APSTA) {
        esp_wifi_connect();
    }
    break;
```

In `wifi_manager_set_mode()`:
```c
app_evt_wifi_mode_changed_t evt = { .mode = mode };
APP_EVENT_POST(APP_EVENT_WIFI_MODE_CHANGED, &evt, sizeof(evt));
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/wifi_manager.h main/wifi_manager.c
git commit -m "refactor: wifi_manager posts WIFI_AP_READY, WIFI_STA_CONNECTED/DISCONNECTED, WIFI_MODE_CHANGED events"
```

---

### Task 14: Refactor `web_server` — Subscribe to Events for SSE, Post Events from Handlers

**Files:**
- Modify: `main/web_server.h`
- Modify: `main/web_server.c`

- [ ] **Step 1: Update `web_server.h`**

Remove:
```c
void web_server_grant_auth(void);
void web_server_notify_switch(uint8_t active_pc);
void web_server_notify_connection(uint8_t pc_id, bool connected);
void web_server_notify_device(const char *device_type, bool connected);
```

- [ ] **Step 2: Update `web_server.c`**

Add `#include "event_bus.h"`.

Remove `#include "switch_manager.h"`, `#include "ble_central.h"`, `#include "ble_peripheral.h"`, `#include "anti_idle.h"`, `#include "input_mode.h"`, `#include "voice_input.h"`, `#include "power_manager.h"`.

Remove `web_server_grant_auth()`, `web_server_notify_switch()`, `web_server_notify_connection()`, `web_server_notify_device()`.

Add SSE event handlers (subscribe to events, push to SSE clients):
```c
static void on_pc_switched(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    app_evt_pc_switched_t *evt = (app_evt_pc_switched_t *)event_data;
    char data[32];
    snprintf(data, sizeof(data), "{\"active_pc\":%d}", evt->new_pc);
    sse_broadcast("switch", data);
}

static void on_pc_connection(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    bool connected = (event_id == APP_EVENT_PC_CONNECTED);
    uint8_t pc_id;
    if (connected) {
        pc_id = ((app_evt_pc_connected_t *)event_data)->pc_id;
    } else {
        pc_id = ((app_evt_pc_disconnected_t *)event_data)->pc_id;
    }
    char data[64];
    snprintf(data, sizeof(data), "{\"pc_id\":%d,\"connected\":%s}",
             pc_id, connected ? "true" : "false");
    sse_broadcast("connection", data);
}

static void on_device_connection(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    const char *device_type = NULL;
    bool connected = true;

    switch (event_id) {
    case APP_EVENT_KB_CONNECTED:  device_type = "keyboard"; break;
    case APP_EVENT_KB_DISCONNECTED: device_type = "keyboard"; connected = false; break;
    case APP_EVENT_MS_CONNECTED:  device_type = "mouse"; break;
    case APP_EVENT_MS_DISCONNECTED: device_type = "mouse"; connected = false; break;
    default: return;
    }

    char data[64];
    snprintf(data, sizeof(data), "{\"device\":\"%s\",\"connected\":%s}",
             device_type, connected ? "true" : "false");
    sse_broadcast("device", data);
}

static void on_web_auth_granted(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    const kvm_config_t *cfg = config_get();
    ESP_LOGI(TAG, "Web access granted");
    char data[128];
    snprintf(data, sizeof(data), "{\"token\":\"%s\"}", cfg->auth_token);
    sse_broadcast("auth", data);
}
```

Register SSE subscriptions in `web_server_start()`:
```c
APP_EVENT_SUBSCRIBE(APP_EVENT_PC_SWITCHED, on_pc_switched, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_PC_CONNECTED, on_pc_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_PC_DISCONNECTED, on_pc_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_KB_CONNECTED, on_device_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_KB_DISCONNECTED, on_device_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_MS_CONNECTED, on_device_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_MS_DISCONNECTED, on_device_connection, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_WEB_AUTH_GRANTED, on_web_auth_granted, NULL);
```

Update `web_server_init()` — replace `wifi_manager_register_ready_cb(web_server_start)`:
```c
void web_server_init(void)
{
    sse_mutex = xSemaphoreCreateMutex();
    memset(sse_clients, 0, sizeof(sse_clients));

    log_mutex = xSemaphoreCreateMutex();
    memset(log_clients, 0, sizeof(log_clients));

    /* Defer httpd_start until AP netif is ready */
    APP_EVENT_SUBSCRIBE(APP_EVENT_WIFI_AP_READY, web_server_start_event_handler, NULL);
}

static void web_server_start_event_handler(void *arg, esp_event_base_t base,
                                            int32_t event_id, void *event_data)
{
    web_server_start();
}
```

Update HTTP handlers to use `config_update_*()` and event posts:

**PATCH /api/settings** — replace all `config_get_mutable()` + `config_save_*()` calls:
```c
/* pc_names */
cJSON *pc_names = cJSON_GetObjectItem(body, "pc_names");
if (pc_names) {
    kvm_config_t *cfg = config_get_mutable();  // still needed for temp mutation
    for (int i = 0; i < MAX_PC_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "pc%d", i + 1);
        cJSON *name = cJSON_GetObjectItem(pc_names, key);
        if (cJSON_IsString(name)) {
            strncpy(cfg->pcs[i].name, name->valuestring, DEVICE_NAME_MAX - 1);
            cfg->pcs[i].name[DEVICE_NAME_MAX - 1] = '\0';
        }
    }
    config_update_blob(CONFIG_FIELD_PC_NAMES, cfg->pcs, sizeof(cfg->pcs));
}

/* device_name */
cJSON *dev_name = cJSON_GetObjectItem(body, "device_name");
if (cJSON_IsString(dev_name)) {
    config_update_str(CONFIG_FIELD_DEVICE_NAME, dev_name->valuestring);
    ble_svc_gap_device_name_set(config_get()->device_name);
}

/* anti_idle */
cJSON *anti_idle = cJSON_GetObjectItem(body, "anti_idle");
if (cJSON_IsBool(anti_idle)) {
    config_update_bool(CONFIG_FIELD_ANTI_IDLE_ENABLED, cJSON_IsTrue(anti_idle));
}

cJSON *anti_idle_ivl = cJSON_GetObjectItem(body, "anti_idle_interval");
if (cJSON_IsNumber(anti_idle_ivl)) {
    config_update_u16(CONFIG_FIELD_ANTI_IDLE_INTERVAL, (uint16_t)anti_idle_ivl->valueint);
}

/* input_mode */
cJSON *im = cJSON_GetObjectItem(body, "input_mode");
if (cJSON_IsNumber(im)) {
    int val = im->valueint;
    if (val >= 0 && val <= 1) {
        input_mode_set((input_mode_t)val);  // this now calls config_update_u8 internally
    }
}

/* air_mouse_sensitivity */
cJSON *sens = cJSON_GetObjectItem(body, "air_mouse_sensitivity");
if (cJSON_IsNumber(sens)) {
    int val = sens->valueint;
    if (val >= 1 && val <= 10) {
        config_update_u8(CONFIG_FIELD_AIR_MOUSE_SENSITIVITY, (uint8_t)val);
    }
}

/* usb_mode */
cJSON *usb_mode_item = cJSON_GetObjectItem(body, "usb_mode");
if (cJSON_IsNumber(usb_mode_item)) {
    int val = usb_mode_item->valueint;
    if (val >= USB_MODE_DISABLED && val <= USB_MODE_HOST) {
        uint8_t old_mode = config_get()->usb_mode;
        config_update_u8(CONFIG_FIELD_USB_MODE, (uint8_t)val);
        if (val != old_mode) {
            /* ... reboot required response ... */
            esp_restart();
            return ESP_OK;
        }
    }
}

/* voice settings */
cJSON *voice_en_item = cJSON_GetObjectItem(body, "voice_asr_enabled");
if (cJSON_IsBool(voice_en_item)) {
    config_update_bool(CONFIG_FIELD_VOICE_ASR_ENABLED, cJSON_IsTrue(voice_en_item));
}

cJSON *voice_appid_item = cJSON_GetObjectItem(body, "voice_asr_appid");
if (cJSON_IsNumber(voice_appid_item)) {
    config_update_u32(CONFIG_FIELD_VOICE_ASR_APPID, (uint32_t)voice_appid_item->valuedouble);
}

cJSON *voice_ak_item = cJSON_GetObjectItem(body, "voice_asr_api_key");
if (cJSON_IsString(voice_ak_item) && strlen(voice_ak_item->valuestring) > 0) {
    if (strncmp(voice_ak_item->valuestring, "****", 4) != 0) {
        config_update_str(CONFIG_FIELD_VOICE_ASR_API_KEY, voice_ak_item->valuestring);
    }
}

cJSON *voice_lang_item = cJSON_GetObjectItem(body, "voice_lang");
if (cJSON_IsString(voice_lang_item)) {
    config_update_str(CONFIG_FIELD_VOICE_LANG, voice_lang_item->valuestring);
}

cJSON *voice_im_item = cJSON_GetObjectItem(body, "voice_input_mode");
if (cJSON_IsNumber(voice_im_item)) {
    int val = voice_im_item->valueint;
    if (val >= 0 && val <= 2) config_update_u8(CONFIG_FIELD_VOICE_INPUT_MODE, (uint8_t)val);
}

/* screen_off_timeout */
cJSON *scr_off = cJSON_GetObjectItem(body, "screen_off_timeout_sec");
if (cJSON_IsNumber(scr_off)) {
    config_update_u16(CONFIG_FIELD_SCREEN_OFF_TIMEOUT, (uint16_t)scr_off->valueint);
}

/* sleep_timeout */
cJSON *sleep_to = cJSON_GetObjectItem(body, "sleep_timeout_sec");
if (cJSON_IsNumber(sleep_to)) {
    config_update_u16(CONFIG_FIELD_SLEEP_TIMEOUT, (uint16_t)sleep_to->valueint);
}

/* keyboard_name */
cJSON *kb_name = cJSON_GetObjectItem(body, "keyboard_name");
if (cJSON_IsString(kb_name)) {
    kvm_config_t *cfg = config_get_mutable();
    strncpy(cfg->keyboard.name, kb_name->valuestring, DEVICE_NAME_MAX - 1);
    cfg->keyboard.name[DEVICE_NAME_MAX - 1] = '\0';
    config_update_blob(CONFIG_FIELD_KEYBOARD_MAC, &cfg->keyboard, sizeof(cfg->keyboard));
}

/* mouse_name */
cJSON *ms_name = cJSON_GetObjectItem(body, "mouse_name");
if (cJSON_IsString(ms_name)) {
    kvm_config_t *cfg = config_get_mutable();
    strncpy(cfg->mouse.name, ms_name->valuestring, DEVICE_NAME_MAX - 1);
    cfg->mouse.name[DEVICE_NAME_MAX - 1] = '\0';
    config_update_blob(CONFIG_FIELD_MOUSE_MAC, &cfg->mouse, sizeof(cfg->mouse));
}
```

**POST /api/pairings** — replace `config_get_mutable()` for device MAC:
```c
if (strcmp(role_item->valuestring, "keyboard") == 0) {
    ble_central_connect_keyboard(addr, (uint8_t)atype_item->valueint);
    kvm_config_t *cfg = config_get_mutable();
    memcpy(cfg->keyboard.mac, addr, 6);
    cfg->keyboard.addr_type = (uint8_t)atype_item->valueint;
    config_update_blob(CONFIG_FIELD_KEYBOARD_MAC, &cfg->keyboard, sizeof(cfg->keyboard));
} else if (strcmp(role_item->valuestring, "mouse") == 0) {
    ble_central_connect_mouse(addr, (uint8_t)atype_item->valueint);
    kvm_config_t *cfg = config_get_mutable();
    memcpy(cfg->mouse.mac, addr, 6);
    cfg->mouse.addr_type = (uint8_t)atype_item->valueint;
    config_update_blob(CONFIG_FIELD_MOUSE_MAC, &cfg->mouse, sizeof(cfg->mouse));
}
```

**POST /api/switch** — replace `switch_manager_request_switch()` + `vTaskDelay`:
```c
static esp_err_t switch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    switch_manager_request_switch();

    vTaskDelay(pdMS_TO_TICKS(150));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "active_pc", switch_manager_get_active_pc());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

**POST /api/pairing/start** — replace `ble_peripheral_enter_pairing_mode()`:
```c
APP_EVENT_POST(APP_EVENT_PAIRING_START, NULL, 0);
```

**POST /api/pairing/stop** — replace `ble_peripheral_exit_pairing_mode()`:
```c
APP_EVENT_POST(APP_EVENT_PAIRING_STOP, NULL, 0);
```

**POST /api/scan** — replace `ble_central_start_scan()`:
```c
APP_EVENT_POST(APP_EVENT_SCAN_START_REQUEST, NULL, 0);
```

**DELETE /api/pairings/{id}** — replace `config_get_mutable()` for PC unpairing:
```c
kvm_config_t *cfg = config_get_mutable();
memset(cfg->pcs[idx].identity_addr, 0, 6);
cfg->pcs[idx].addr_type = 0;
cfg->pcs[idx].name[0] = '\0';
cfg->pcs[idx].conn_handle = 0;
cfg->pcs[idx].connected = false;
config_update_blob(CONFIG_FIELD_PC_NAMES, cfg->pcs, sizeof(cfg->pcs));
```

- [ ] **Step 3: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/web_server.h main/web_server.c
git commit -m "refactor: web_server subscribes to events for SSE, posts events from HTTP handlers"
```

---

### Task 15: Refactor `main.c` — Simplify Init, Remove Callback Wiring

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Update `main.c`**

Add `#include "event_bus.h"`.

Remove `#include "anti_idle.h"` (no longer needed — anti_idle self-registers).

Remove the `on_pc_conn_event()` callback function.

Remove the callback wiring:
```c
// REMOVE:
hid_router_register_activity_cb(anti_idle_on_activity);
ble_peripheral_register_conn_cb(on_pc_conn_event);
```

Updated `app_main()`:
```c
void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    /* Step 1: Storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Step 2: Event bus — must be before any module that uses events */
    event_bus_init();

    /* Step 3: Config */
    config_manager_init();
    web_log_init();

    /* Step 4: Hardware */
    indicator_init();

#if HAS_BATTERY || HAS_INPUT_MODES || HAS_VOICE_INPUT
    extern i2c_master_bus_handle_t tft_display_get_i2c_bus(void);
    i2c_master_bus_handle_t i2c_bus = tft_display_get_i2c_bus();
#endif

#if HAS_BATTERY
    power_manager_init(i2c_bus);
    pm_sleep_init();
#endif

#if HAS_INPUT_MODES
    imu_driver_init(i2c_bus);
    input_mode_init();
#endif

    const kvm_config_t *cfg = config_get();

#if HAS_VOICE_INPUT
    if (cfg->voice_asr_enabled && cfg->voice_asr_appid != 0) {
        es8311_init(i2c_bus);
        mic_driver_init();
        voice_input_init();
        ESP_LOGI(TAG, "Voice input initialized");
    }
#endif

    /* Step 5: Network */
    web_server_init();
    wifi_manager_init();

    /* Step 6: USB */
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_DEVICE) {
        usb_device_init();
        ESP_LOGI(TAG, "USB Device mode active");
    } else if (cfg->usb_mode == USB_MODE_HOST) {
        usb_host_init(hid_router_on_usb_keyboard, hid_router_on_usb_mouse);
        ESP_LOGI(TAG, "USB Host mode active");
    } else {
        ESP_LOGI(TAG, "USB disabled (BLE-only)");
    }
#endif

    /* Step 7: BLE */
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_peripheral_init();
    if (cfg->usb_mode != USB_MODE_HOST) {
        ble_central_init();
    }
    nimble_port_freertos_init(ble_host_task);

    /* Step 8: Input routing — modules self-register event subscriptions */
    switch_manager_init();
    hid_router_init();
    anti_idle_init();

    /* Step 9: Subscribe to FACTORY_RESET event */
    APP_EVENT_SUBSCRIBE(APP_EVENT_FACTORY_RESET, on_factory_reset, NULL);

    ESP_LOGI(TAG, "BLE-KVM initialized, usb_mode: %d", cfg->usb_mode);
}
```

Add factory reset handler:
```c
static void on_factory_reset(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    ESP_LOGW(TAG, "Factory reset triggered!");
    nvs_flash_erase();
    esp_restart();
}
```

Also add scan start handler (or have `ble_central` subscribe directly):
```c
/* In ble_central_init(), add: */
APP_EVENT_SUBSCRIBE(APP_EVENT_SCAN_START_REQUEST, on_scan_start_request, NULL);

static void on_scan_start_request(void *arg, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
{
    ble_central_start_scan();
}
```

And pairing start/stop handlers in `ble_peripheral_init()`:
```c
APP_EVENT_SUBSCRIBE(APP_EVENT_PAIRING_START, on_pairing_start, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_PAIRING_STOP, on_pairing_stop, NULL);

static void on_pairing_start(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    ble_peripheral_enter_pairing_mode();
}

static void on_pairing_stop(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    ble_peripheral_exit_pairing_mode();
}
```

- [ ] **Step 2: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add main/main.c
git commit -m "refactor: main.c simplified — modules self-register, no callback wiring"
```

---

### Task 16: Subscribe `ble_peripheral` to HID Forward + Consumer Key Events

**Files:**
- Modify: `main/ble_peripheral.c`

- [ ] **Step 1: Add event subscriptions**

In `ble_peripheral_init()`, add:
```c
APP_EVENT_SUBSCRIBE(APP_EVENT_HID_FORWARD_KEYBOARD, on_hid_forward_keyboard, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_HID_FORWARD_MOUSE, on_hid_forward_mouse, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_HID_CONSUMER_KEY, on_hid_consumer_key, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_HID_ANTI_IDLE_NUDGE, on_anti_idle_nudge, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_PAIRING_START, on_pairing_start, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_PAIRING_STOP, on_pairing_stop, NULL);
APP_EVENT_SUBSCRIBE(APP_EVENT_CONFIG_CHANGED, on_config_changed, NULL);
```

Add handlers:
```c
static void on_hid_forward_keyboard(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    const kvm_config_t *cfg = config_get();
    uint16_t conn_handle = switch_manager_get_active_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    ble_peripheral_send_hid_report(conn_handle, 1, evt->data, evt->len);
}

static void on_hid_forward_mouse(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    const kvm_config_t *cfg = config_get();
    uint16_t conn_handle = switch_manager_get_active_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    ble_peripheral_send_hid_report(conn_handle, 2, evt->data, evt->len);
}

static void on_hid_consumer_key(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    app_evt_consumer_key_t *evt = (app_evt_consumer_key_t *)event_data;
    ble_peripheral_send_consumer_key(evt->conn_handle, evt->usage_code);
}

static void on_anti_idle_nudge(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    app_evt_anti_idle_nudge_t *evt = (app_evt_anti_idle_nudge_t *)event_data;
    uint16_t conn = ble_peripheral_get_conn_handle(evt->pc_id - 1);
    if (conn == 0 || conn == 0xFFFF) return;
    uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
    ble_peripheral_send_hid_report(conn, 2, report1, sizeof(report1));
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
    ble_peripheral_send_hid_report(conn, 2, report2, sizeof(report2));
}

static void on_pairing_start(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    ble_peripheral_enter_pairing_mode();
}

static void on_pairing_stop(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    ble_peripheral_exit_pairing_mode();
}

static void on_config_changed(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    app_evt_config_changed_t *evt = (app_evt_config_changed_t *)event_data;
    if (evt->field == CONFIG_FIELD_DEVICE_NAME) {
        ble_svc_gap_device_name_set(config_get()->device_name);
    }
}
```

- [ ] **Step 2: Build verification**

```bash
cd /home/gem/project/ble-kvm
idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add main/ble_peripheral.c
git commit -m "refactor: ble_peripheral subscribes to HID forward, consumer key, pairing, config events"
```

---

### Task 17: Final Integration — Build, Flash, and Verify

**Files:**
- Modify: `main/CMakeLists.txt` (already done in Task 2)

- [ ] **Step 1: Full clean build**

```bash
cd /home/gem/project/ble-kvm
idf.py fullclean
idf.py build 2>&1 | tail -40
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Check for remaining `config_get_mutable()` calls**

```bash
grep -rn "config_get_mutable" main/*.c
```

Expected: Only in `config_manager.c` (the implementation) and possibly `web_server.c` for temporary mutation before `config_update_blob()`. Any other hits need to be fixed.

- [ ] **Step 3: Check for remaining `extern` declarations in .c files**

```bash
grep -rn "^extern " main/*.c
```

Expected: Only `extern i2c_master_bus_handle_t tft_display_get_i2c_bus(void)` in `main.c` (needed because main.c doesn't include tft_display.h directly — or we can add the include).

- [ ] **Step 4: Flash and verify**

```bash
idf.py flash monitor
```

Verify:
- Device boots without errors
- Web server starts (check for "Web server started on port 80")
- BLE advertising starts
- Button presses work (switch, secondary)
- Web UI loads and shows status
- SSE events work (switch PC, see notification in browser)

- [ ] **Step 5: Commit any final fixes**

```bash
git add -A
git commit -m "fix: final integration fixes for event-driven refactoring"
```
