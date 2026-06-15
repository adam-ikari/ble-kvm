#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_event.h"

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
