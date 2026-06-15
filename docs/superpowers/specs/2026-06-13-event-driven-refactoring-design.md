# BLE-KVM Event-Driven Architecture Refactoring

> **Status:** Design spec — brainstorming phase complete, plan phase next
> **Date:** 2026-06-13
> **Scope:** Full system refactoring — every module converted to event-driven communication

## Goal

Replace all direct cross-module function calls with an `esp_event`-based event bus. Every module publishes events when its state changes and subscribes to events it cares about. No module directly calls another module's functions except read-only query getters.

## Architecture

A single `esp_event_loop` (`app_event_loop`) serves as the system-wide event bus. Each module in `app_main()` registers its event subscriptions during `_init()`, publishes events when its state changes, and consumes events from other modules via registered handlers.

**HID data path:** Keyboard and mouse HID reports flow through `esp_event` synchronously — the callback runs in the caller's FreeRTOS task context with no extra context switch, no heap allocation, and no data copy (pointer pass). The latency is identical to the current direct function call approach.

## Event Catalog

All events use a single ESP-IDF event base `APP_EVENT`. The event ID determines the payload type.

### Connection Events
| Event ID | Payload | Publisher | Subscribers |
|----------|---------|-----------|-------------|
| `PC_CONNECTED` | `{uint8_t pc_id, uint16_t conn_handle}` | `ble_peripheral` | `switch_manager`, `power_manager`, `anti_idle`, `web_server` |
| `PC_DISCONNECTED` | `{uint8_t pc_id}` | `ble_peripheral` | `switch_manager`, `power_manager`, `web_server` |
| `USB_DEVICE_CONNECTED` | `{}` | `usb_device` | `switch_manager`, `web_server` |
| `USB_DEVICE_DISCONNECTED` | `{}` | `usb_device` | `switch_manager`, `web_server` |
| `KB_CONNECTED` | `{uint16_t conn_handle}` | `ble_central` | `web_server`, `anti_idle` |
| `KB_DISCONNECTED` | `{}` | `ble_central` | `web_server`, `anti_idle` |
| `MS_CONNECTED` | `{uint16_t conn_handle}` | `ble_central` | `web_server`, `anti_idle` |
| `MS_DISCONNECTED` | `{}` | `ble_central` | `web_server`, `anti_idle` |

### HID Data Events
| Event ID | Payload | Publisher | Subscribers |
|----------|---------|-----------|-------------|
| `HID_KEYBOARD_DATA` | `{const uint8_t *data, uint8_t len}` | `ble_central`, `usb_host` | `hid_router` |
| `HID_MOUSE_DATA` | `{const uint8_t *data, uint8_t len}` | `ble_central`, `usb_host` | `hid_router` |
| `HID_FORWARD_KEYBOARD` | `{const uint8_t *data, uint8_t len}` | `hid_router` | `ble_peripheral`, `usb_device` |
| `HID_FORWARD_MOUSE` | `{const uint8_t *data, uint8_t len}` | `hid_router` | `ble_peripheral`, `usb_device` |
| `HID_ACTIVITY` | `{}` | `hid_router` | `anti_idle`, `power_manager` |
| `HID_ANTI_IDLE_NUDGE` | `{uint8_t pc_id}` | `anti_idle` | `ble_peripheral`, `usb_device` |
| `HID_CONSUMER_KEY` | `{uint16_t conn_handle, uint16_t usage}` | `input_mode`, `switch_manager` | `ble_peripheral` |

### Input Events
| Event ID | Payload | Publisher | Subscribers |
|----------|---------|-----------|-------------|
| `INPUT_BUTTON_PRIMARY` | `{button_event_t type}` | `switch_manager` | (handled internally by switch_manager) |
| `INPUT_BUTTON_SECONDARY` | `{button_event_t type}` | `switch_manager` | (handled internally) |
| `INPUT_MODE_CHANGED` | `{input_mode_t old, input_mode_t new}` | `input_mode` | `switch_manager`, `web_server` |
| `INPUT_AIR_MOUSE` | `{int8_t x, int8_t y}` | `input_mode` | `ble_peripheral` |
| `INPUT_IMU_MOTION` | `{}` | `input_mode` | `power_manager` |

### System/Config Events
| Event ID | Payload | Publisher | Subscribers |
|----------|---------|-----------|-------------|
| `CONFIG_CHANGED` | `{config_field_t field}` | `config_manager` | `ble_peripheral`, `wifi_manager`, `anti_idle`, `power_manager`, `web_server` |
| `PC_SWITCHED` | `{uint8_t old_pc, uint8_t new_pc}` | `switch_manager` | `web_server` |
| `FACTORY_RESET` | `{}` | `switch_manager`, `web_server` | (handled in main.c — calls nvs_flash_erase + esp_restart) |
| `WEB_AUTH_GRANTED` | `{}` | `switch_manager` | `web_server` |
| `PAIRING_START` | `{}` | `web_server`, `switch_manager` | `ble_peripheral` |
| `PAIRING_STOP` | `{}` | `web_server`, `ble_peripheral` | (none — indicator called directly) |
| `VOICE_START_REQUEST` | `{}` | `switch_manager` | `voice_input` |
| `VOICE_STOP_REQUEST` | `{}` | `switch_manager` | `voice_input` |
| `VOICE_STATE_CHANGED` | `{bool active}` | `voice_input` | `web_server` |
| `WIFI_MODE_CHANGED` | `{wifi_operating_mode_t mode}` | `wifi_manager` | `web_server`, `power_manager` |
| `WIFI_STA_CONNECTED` | `{char ip[16]}` | `wifi_manager` | `web_server`, `voice_input` |
| `WIFI_STA_DISCONNECTED` | `{}` | `wifi_manager` | `web_server` |
| `SLEEP_STATE_CHANGED` | `{pm_sleep_state_t state}` | `power_manager` | `web_server`, `anti_idle` |
| `SCR_OFF_STATE_CHANGED` | `{bool off}` | `power_manager` | (none — indicator called directly) |

## Module Responsibility Changes

### `event_bus.h` — NEW FILE
Defines the event base, all event IDs, and all event payload structs. Every module includes this single header.

### `event_bus.c` — NEW FILE
Creates the `app_event_loop` and provides convenience functions:
- `event_bus_init(void)` — creates the loop, called early in `app_main()`
- `EVENT_POST(id, data)` — macro wrapping `esp_event_post_to()`
- `EVENT_SUBSCRIBE(id, handler)` — macro wrapping `esp_event_handler_register_with()`

### `config_manager.c` — MODIFIED
- **REMOVED:** `config_get_mutable()` — no more direct mutation
- **ADDED:** `config_update_u8(ConfigField field, uint8_t value)` — validates, updates NVS, posts `CONFIG_CHANGED`
- **ADDED:** `config_update_u16(ConfigField field, uint16_t value)` 
- **ADDED:** `config_update_str(ConfigField field, const char *value)` 
- **ADDED:** `config_update_bool(ConfigField field, bool value)` 
- **ADDED:** `config_update_blob(ConfigField field, const void *data, size_t len)` 
- `config_get()` remains (const, read-only queries are fine)
- Modules that were mutating config directly now call `config_update_*()` instead

### `switch_manager.c` — SIMPLIFIED
- Button ISR → queue → task loop remains (internal implementation)
- Instead of calling 30+ cross-module functions, the task loop **posts events**:
  - CMD_SWITCH → post `PC_SWITCHED`
  - CMD_SECONDARY → post `INPUT_BUTTON_SECONDARY`
  - CMD_FACTORY_RST → post `FACTORY_RESET`
  - CMD_WEB_AUTH → post `WEB_AUTH_GRANTED`
  - CMD_PPT_PAGE_UP → post `HID_CONSUMER_KEY`
  - CMD_VOICE_START → post `VOICE_START_REQUEST`
  - CMD_VOICE_STOP → post `VOICE_STOP_REQUEST`
- Subscribes to `PC_CONNECTED`, `PC_DISCONNECTED`, `USB_DEVICE_CONNECTED`, `USB_DEVICE_DISCONNECTED` → calls `update_led_state()`
- Subscribes to `INPUT_MODE_CHANGED` → may adjust LED
- `switch_manager_on_pc_connected()` and `switch_manager_on_pc_disconnected()` are **removed** — the BLE gap event handler posts events instead
- Query functions (`switch_manager_get_active_pc()`, `switch_manager_get_active_conn_handle()`) remain — they're read-only

### `hid_router.c` — BECOMES EVENT-MEDIATED
- Subscribes to `HID_KEYBOARD_DATA`, `HID_MOUSE_DATA`
- Determines routing (USB vs BLE) and posts `HID_FORWARD_KEYBOARD` or `HID_FORWARD_MOUSE`
- Posts `HID_ACTIVITY` on every forward
- `hid_router_register_activity_cb()` is **removed** — modules subscribe to `HID_ACTIVITY` instead

### `ble_peripheral.c` — MINIMAL CHANGES
- GAP event handler posts `PC_CONNECTED` / `PC_DISCONNECTED` instead of calling `conn_cb`
- Subscribes to `HID_FORWARD_KEYBOARD`, `HID_FORWARD_MOUSE`, `HID_CONSUMER_KEY`, `HID_ANTI_IDLE_NUDGE`
- Subscribes to `PAIRING_START`, `PAIRING_STOP` (for pairing mode)
- Subscribes to `CONFIG_CHANGED` (for device name updates)
- `ble_peripheral_register_conn_cb()` is **removed**
- Query functions (`ble_peripheral_is_pc_connected()`, `ble_peripheral_get_conn_handle()`) remain

### `ble_central.c` — MINIMAL CHANGES
- GAP event handler posts `KB_CONNECTED`, `KB_DISCONNECTED`, `MS_CONNECTED`, `MS_DISCONNECTED`
- NOTIFY_RX handler posts `HID_KEYBOARD_DATA`, `HID_MOUSE_DATA` instead of calling `hid_router_forward_*()` directly
- Query functions (`ble_central_is_keyboard_connected()`, etc.) remain

### `web_server.c` — BECOMES PURE EVENT CONSUMER/PRODUCER
- HTTP handlers post events instead of calling module functions:
  - POST /api/switch → post `PC_SWITCH_REQUEST`
  - PATCH /api/settings → `config_update_*()` + post events for non-config actions
  - POST /api/pairings → post `PAIRING_*`, `KB_CONNECT_REQUEST`, etc.
  - POST /api/pairing/start → post `PAIRING_START`
  - POST /api/pairing/stop → post `PAIRING_STOP`
  - POST /api/scan → post `SCAN_START_REQUEST`
- Subscribes to `PC_SWITCHED`, `PC_CONNECTED`, `PC_DISCONNECTED`, `KB_CONNECTED`, `KB_DISCONNECTED`, `MS_CONNECTED`, `MS_DISCONNECTED`, `WIFI_*`, `SLEEP_*`, `VOICE_*`, `INPUT_MODE_CHANGED` → pushes SSE notifications
- Subscribes to `WEB_AUTH_GRANTED` → pushes auth token via SSE
- `web_server_notify_switch()`, `web_server_notify_connection()`, `web_server_notify_device()` are **removed**
- Status endpoint still uses read-only query functions (`config_get()`, `wifi_manager_get_mode()`, etc.) — no change needed

### `anti_idle.c` — SIMPLIFIED
- Subscribes to `HID_ACTIVITY` instead of being called via callback
- Subscribes to `PC_CONNECTED` instead of `anti_idle_on_pc_connected()`
- Subscribes to `CONFIG_CHANGED` (for anti_idle_enabled, anti_idle_interval)
- Posts `HID_ANTI_IDLE_NUDGE` for each PC that needs a nudge
- Timer callback remains (internal implementation)
- `anti_idle_on_activity()`, `anti_idle_on_pc_connected()`, `anti_idle_set_enabled()`, `anti_idle_set_interval()` are **removed** or become static

### `power_manager.c` — SIMPLIFIED
- Subscribes to `HID_ACTIVITY`, `PC_CONNECTED`, `PC_DISCONNECTED`, `INPUT_IMU_MOTION`
- Subscribes to `CONFIG_CHANGED` (for screen_off_timeout, sleep_timeout)
- Posts `SLEEP_STATE_CHANGED`, `SCR_OFF_STATE_CHANGED`
- `pm_sleep_on_activity()`, `pm_sleep_on_pc_connected()`, `pm_sleep_on_pc_disconnected()`, `pm_sleep_on_imu_motion()` are **removed** or become static
- Internal timer callbacks remain

### `input_mode.c` — SIMPLIFIED
- Subscribes to `INPUT_BUTTON_PRIMARY`, `INPUT_BUTTON_SECONDARY` (for non-KVM modes)
- Posts `INPUT_MODE_CHANGED` on mode switch
- Posts `INPUT_AIR_MOUSE` for air mouse data
- Posts `INPUT_IMU_MOTION` when IMU detects movement (for power manager sleep timer reset)
- Air mouse task remains (internal implementation)

### `voice_input.c` — UNCHANGED LOGIC
- Subscribes to `VOICE_START_REQUEST`, `VOICE_STOP_REQUEST`
- Posts `VOICE_STATE_CHANGED`
- Internal WebSocket/streaming logic unchanged

### `indicator.c/h` — UNCHANGED
- `indicator_set_state()` remains a public function — it's a hardware actuator (set LED/RGB), not cross-module business logic. Adding events for LED control is over-engineering.
- Callers (`switch_manager`, `ble_peripheral` for pairing, `power_manager` for sleep) continue to call it directly.

### `wifi_manager.c` — MINIMAL CHANGES
- Already event-driven internally (esp_event for WiFi/IP)
- Posts `WIFI_MODE_CHANGED`, `WIFI_STA_CONNECTED`, `WIFI_STA_DISCONNECTED`
- Subscribes to `CONFIG_CHANGED` (for wifi_ssid, wifi_password changes)
- `wifi_manager_register_ready_cb()` remains (web_server startup still depends on AP start)
- Query functions remain

### `main.c` — SIMPLIFIED
```c
void app_main(void) {
    nvs_flash_init();           // Step 1: storage
    event_bus_init();           // Step 2: create app event loop
    config_manager_init();       // Step 3: load config
    web_log_init();              // Step 4: log hook
    indicator_init();            // Step 5: TFT + I2C bus
    // I2C peripherals
    power_manager_init(i2c_bus);
    pm_sleep_init();
    imu_driver_init(i2c_bus);
    input_mode_init();
    // Voice (conditional)
    es8311_init(i2c_bus);
    mic_driver_init();
    voice_input_init();
    // Network
    web_server_init();
    wifi_manager_init();
    // USB
    usb_device_init() or usb_host_init();
    // BLE
    nimble_port_init();
    ble_peripheral_init();
    ble_central_init();
    nimble_port_freertos_init(ble_host_task);
    // Input routing
    switch_manager_init();
    hid_router_init();
    anti_idle_init();
    // Done — all cross-module wiring is via event subscriptions
}
```

**Key change:** `main.c` no longer wires callbacks between modules (`hid_router_register_activity_cb`, `ble_peripheral_register_conn_cb`). Modules self-register their event subscriptions in their own `_init()` functions.

## Init Order Dependencies

The topological sort of init dependencies:

```
nvs_flash_init
├── event_bus_init          (needs: nothing)
├── config_manager_init     (needs: nvs)
├── web_log_init            (needs: nothing — hooks esp_log)
├── indicator_init          (needs: nvs — creates I2C bus)
│   ├── power_manager_init  (needs: i2c_bus from indicator)
│   ├── pm_sleep_init       (needs: config, power_manager)
│   ├── imu_driver_init     (needs: i2c_bus)
│   └── input_mode_init     (needs: imu_driver, config)
├── es8311_init             (needs: i2c_bus)
├── mic_driver_init         (needs: es8311)
├── voice_input_init        (needs: config)
├── web_server_init         (needs: config — registers wifi ready cb)
├── wifi_manager_init       (needs: config — fires ready cb → web_server_start)
├── usb_device/host_init    (needs: config)
├── nimble_port_init        (needs: nothing)
├── ble_peripheral_init     (needs: config)
├── ble_central_init        (needs: config)
├── nimble_port_freertos_init (needs: all BLE inits)
├── switch_manager_init     (needs: event_bus, nimble loop active, GPIO)
├── hid_router_init         (needs: event_bus)
└── anti_idle_init          (needs: event_bus, config)
```

## Extern Elimination

All 5 `extern` declarations are eliminated:

| Current extern | New location |
|----------------|-------------|
| `extern tft_display_get_i2c_bus()` | Already in `tft_display.h` (public) |
| `extern tft_display_toggle_page()` | Add to `tft_display.h` |
| `extern tft_display_get_panel()` | Add to `tft_display.h` |
| `extern tft_display_freeze()` | Add to `tft_display.h` |
| `extern web_server_grant_auth()` | Replaced by `WEB_AUTH_GRANTED` event subscription |

## What Does NOT Change

1. **FreeRTOS tasks** — all 7 tasks remain (ble_host, switch_mgr, tft_disp, air_mouse, voice_in, rgb_led, usb_host_lib). The event bus callbacks execute in the caller's task context.
2. **GPIO ISRs** — unchanged. They continue to post to `switch_queue` (FreeRTOS queue, not event bus — ISR context cannot use `esp_event_post`).
3. **BLE NimBLE event loop** — unchanged. The NimBLE event handler posts to our `app_event_loop`.
4. **WiFi/IP esp_event handlers** — unchanged. WiFi events continue on the default event loop. Our `app_event_loop` is separate.
5. **Read-only query functions** — all getter functions remain. Querying state is not coupling.
6. **HID data latency** — identical to current path. `esp_event` callbacks are synchronous in the caller's task context.

## Non-Goals (Out of Scope)

- Replacing FreeRTOS tasks with event-driven state machines — tasks are fine
- Changing the SPI/I2C driver layer
- Changing the NimBLE integration
- Changing the WebSocket/HTTP client in voice_input
- Adding a formal init framework with dependency resolution — the init order in main.c with comments is sufficient for now
