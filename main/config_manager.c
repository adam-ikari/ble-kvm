#include "config_manager.h"
#include "event_bus.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "config";
static kvm_config_t config;
static nvs_handle_t nvs_ble;
static nvs_handle_t nvs_config;
static nvs_handle_t nvs_wifi;
static nvs_handle_t nvs_system;

static const char *NS_BLE = "kvm_ble";
static const char *NS_CONFIG = "kvm_config";
static const char *NS_WIFI = "kvm_wifi";
static const char *NS_SYSTEM = "kvm_system";

static void load_pcs(void)
{
    size_t required_size = sizeof(config.pcs);
    esp_err_t err = nvs_get_blob(nvs_ble, "pcs", config.pcs, &required_size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved PCs, using defaults");
        memset(config.pcs, 0, sizeof(config.pcs));
        config.pcs[0].pc_id = 1;
        config.pcs[1].pc_id = 2;
        config.pcs[2].pc_id = 3;
    }
}

static void load_input_devices(void)
{
    size_t required_size = sizeof(config.keyboard);
    esp_err_t err = nvs_get_blob(nvs_ble, "keyboard", &config.keyboard, &required_size);
    if (err != ESP_OK) {
        memset(&config.keyboard, 0, sizeof(config.keyboard));
    }
    required_size = sizeof(config.mouse);
    err = nvs_get_blob(nvs_ble, "mouse", &config.mouse, &required_size);
    if (err != ESP_OK) {
        memset(&config.mouse, 0, sizeof(config.mouse));
    }
}

static void load_active_pc(void)
{
    esp_err_t err = nvs_get_u8(nvs_config, "active_pc", &config.active_pc);
    if (err != ESP_OK) {
        config.active_pc = 1;
    }
}

static void load_auth_token(void)
{
    size_t required_size = sizeof(config.auth_token);
    esp_err_t err = nvs_get_str(nvs_config, "auth_token", config.auth_token, &required_size);
    if (err != ESP_OK) {
        config_generate_auth_token();
    }
}

static void load_wifi(void)
{
    size_t required_size = sizeof(config.wifi_ssid);
    esp_err_t err = nvs_get_str(nvs_wifi, "ssid", config.wifi_ssid, &required_size);
    if (err != ESP_OK) {
        config.wifi_ssid[0] = '\0';
    }
    required_size = sizeof(config.wifi_password);
    err = nvs_get_str(nvs_wifi, "password", config.wifi_password, &required_size);
    if (err != ESP_OK) {
        config.wifi_password[0] = '\0';
    }
    uint8_t enabled = 0;
    err = nvs_get_u8(nvs_wifi, "enabled", &enabled);
    if (err != ESP_OK) {
        config.wifi_enabled = true;
    } else {
        config.wifi_enabled = enabled ? true : false;
    }
}

static void load_anti_idle(void);

static void load_input_mode(void);

static void load_usb_mode(void);

static void load_voice(void);

static void load_sleep(void);

static void load_device_name(void);

void config_manager_init(void)
{
    ESP_ERROR_CHECK(nvs_open(NS_BLE, NVS_READWRITE, &nvs_ble));
    ESP_ERROR_CHECK(nvs_open(NS_CONFIG, NVS_READWRITE, &nvs_config));
    ESP_ERROR_CHECK(nvs_open(NS_WIFI, NVS_READWRITE, &nvs_wifi));
    ESP_ERROR_CHECK(nvs_open(NS_SYSTEM, NVS_READWRITE, &nvs_system));

    load_pcs();
    load_input_devices();
    load_active_pc();
    load_auth_token();
    load_wifi();
    load_anti_idle();
    load_input_mode();
    load_usb_mode();
    load_voice();
    load_sleep();
    load_device_name();

    ESP_LOGI(TAG, "Config loaded: active_pc=%d, auth_token=%s", config.active_pc, config.auth_token);
}

const kvm_config_t *config_get(void)
{
    return &config;
}

kvm_config_t *config_get_mutable(void)
{
    return &config;
}

void config_generate_auth_token(void)
{
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < AUTH_TOKEN_LEN - 1; i++) {
        config.auth_token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    config.auth_token[AUTH_TOKEN_LEN - 1] = '\0';

    /* Save to NVS first, then notify subscribers */
    nvs_set_str(nvs_config, "auth_token", config.auth_token);
    nvs_commit(nvs_config);
    ESP_LOGI(TAG, "Generated auth token: %s", config.auth_token);

    app_evt_config_changed_t evt = { .field = CONFIG_FIELD_AUTH_TOKEN };
    APP_EVENT_POST(APP_EVENT_CONFIG_CHANGED, &evt, sizeof(evt));
}

static void load_anti_idle(void)
{
    uint8_t enabled = 0;
    esp_err_t err = nvs_get_u8(nvs_config, "anti_idle", &enabled);
    config.anti_idle_enabled = (err == ESP_OK) ? (enabled ? true : false) : false;

    uint16_t interval = 0;
    err = nvs_get_u16(nvs_config, "anti_idle_ivl", &interval);
    config.anti_idle_interval_sec = (err == ESP_OK) ? interval : 240;
}

static void load_input_mode(void)
{
    esp_err_t err = nvs_get_u8(nvs_config, "input_mode", &config.input_mode);
    if (err != ESP_OK || config.input_mode > 1) config.input_mode = 0;
    uint8_t sens = 0;
    err = nvs_get_u8(nvs_config, "air_sens", &sens);
    config.air_mouse_sensitivity = (err == ESP_OK && sens >= 1 && sens <= 10) ? sens : 5;
}

static void load_usb_mode(void)
{
    esp_err_t err = nvs_get_u8(nvs_config, "usb_mode", &config.usb_mode);
    if (err != ESP_OK || config.usb_mode > USB_MODE_HOST) {
        config.usb_mode = USB_MODE_DISABLED;
    }
}

static void load_voice(void)
{
    uint8_t enabled = 0;
    esp_err_t err = nvs_get_u8(nvs_config, "voice_en", &enabled);
    config.voice_asr_enabled = (err == ESP_OK) ? (enabled ? true : false) : false;

    err = nvs_get_u32(nvs_config, "voice_appid", &config.voice_asr_appid);
    if (err != ESP_OK) config.voice_asr_appid = 0;

    size_t required_size = sizeof(config.voice_asr_api_key);
    err = nvs_get_str(nvs_config, "voice_ak", config.voice_asr_api_key, &required_size);
    if (err != ESP_OK) config.voice_asr_api_key[0] = '\0';

    required_size = sizeof(config.voice_lang);
    err = nvs_get_str(nvs_config, "voice_lang", config.voice_lang, &required_size);
    if (err != ESP_OK) {
        strncpy(config.voice_lang, "zh", sizeof(config.voice_lang));
    }

    uint8_t im = 0;
    err = nvs_get_u8(nvs_config, "voice_im", &im);
    config.voice_input_mode = (err == ESP_OK && im <= 2) ? im : 0;
}

static void load_sleep(void)
{
    uint16_t val = 0;
    esp_err_t err = nvs_get_u16(nvs_config, "scr_off_to", &val);
    config.screen_off_timeout_sec = (err == ESP_OK) ? val : 120;

    val = 0;
    err = nvs_get_u16(nvs_config, "sleep_to", &val);
    config.sleep_timeout_sec = (err == ESP_OK) ? val : 300;
}

static void load_device_name(void)
{
    size_t required_size = sizeof(config.device_name);
    esp_err_t err = nvs_get_str(nvs_config, "dev_name", config.device_name, &required_size);
    if (err != ESP_OK) {
        config.device_name[0] = '\0';
    }
}

/* ── config_update_*() API ──────────────────────────────────────────────── */

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

void config_manager_deinit(void)
{
    nvs_close(nvs_ble);
    nvs_close(nvs_config);
    nvs_close(nvs_wifi);
    nvs_close(nvs_system);
}
