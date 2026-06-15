#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "event_bus.h"

#define MAX_PC_COUNT 3
#define DEVICE_NAME_MAX 32
#define AUTH_TOKEN_LEN 9

#define USB_MODE_DISABLED  0
#define USB_MODE_DEVICE    1
#define USB_MODE_HOST      2

typedef struct {
    uint8_t identity_addr[6];
    uint8_t addr_type;
    uint8_t pc_id;
    char name[DEVICE_NAME_MAX];
    uint16_t conn_handle;
    bool connected;
} pc_device_t;

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    bool is_combo;
    char name[DEVICE_NAME_MAX];
    bool connected;
} input_device_t;

typedef struct {
    pc_device_t pcs[MAX_PC_COUNT];
    input_device_t keyboard;
    input_device_t mouse;
    uint8_t active_pc;
    uint8_t usb_mode;               /* 0=disabled, 1=device, 2=host */
    char auth_token[AUTH_TOKEN_LEN];
    char wifi_ssid[33];
    char wifi_password[65];
    bool wifi_enabled;
    bool anti_idle_enabled;
    uint16_t anti_idle_interval_sec;
    uint16_t screen_off_timeout_sec;   /* seconds, 0 = never, default 30, battery only */
    uint16_t sleep_timeout_sec;        /* seconds, 0 = never, default 60, battery only */
    uint8_t input_mode;            /* 0=KVM, 1=PPT/Air Mouse */
    uint8_t air_mouse_sensitivity; /* 1-10, default 5 */
    bool voice_asr_enabled;           /* default: false */
    uint32_t voice_asr_appid;         /* Baidu App ID */
    char voice_asr_api_key[65];       /* Baidu API Key (appkey) */
    char voice_lang[8];               /* "zh" or "en", default: "zh" */
    uint8_t voice_input_mode;         /* 0=auto, 1=pinyin, 2=ascii */
    char device_name[DEVICE_NAME_MAX];  /* BLE broadcast name, default KVM-<MAC> */
} kvm_config_t;

void config_manager_init(void);
const kvm_config_t *config_get(void);
kvm_config_t *config_get_mutable(void);
void config_generate_auth_token(void);
void config_manager_deinit(void);

/* New config_update_*() API — validates, updates in-memory config, saves to NVS, posts APP_EVENT_CONFIG_CHANGED */
void config_update_u8(config_field_t field, uint8_t value);
void config_update_u16(config_field_t field, uint16_t value);
void config_update_u32(config_field_t field, uint32_t value);
void config_update_bool(config_field_t field, bool value);
void config_update_str(config_field_t field, const char *value);
void config_update_blob(config_field_t field, const void *data, size_t len);
