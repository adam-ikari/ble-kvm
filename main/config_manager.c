#include "config_manager.h"
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
        config_save_auth_token();
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

void config_save_pcs(void)
{
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "pcs", config.pcs, sizeof(config.pcs)));
    ESP_ERROR_CHECK(nvs_commit(nvs_ble));
}

void config_save_input_devices(void)
{
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "keyboard", &config.keyboard, sizeof(config.keyboard)));
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "mouse", &config.mouse, sizeof(config.mouse)));
    ESP_ERROR_CHECK(nvs_commit(nvs_ble));
}

void config_save_active_pc(void)
{
    ESP_ERROR_CHECK(nvs_set_u8(nvs_config, "active_pc", config.active_pc));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}

void config_save_auth_token(void)
{
    ESP_ERROR_CHECK(nvs_set_str(nvs_config, "auth_token", config.auth_token));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}

void config_save_wifi(void)
{
    ESP_ERROR_CHECK(nvs_set_str(nvs_wifi, "ssid", config.wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_wifi, "password", config.wifi_password));
    uint8_t enabled = config.wifi_enabled ? 1 : 0;
    ESP_ERROR_CHECK(nvs_set_u8(nvs_wifi, "enabled", enabled));
    ESP_ERROR_CHECK(nvs_commit(nvs_wifi));
}

void config_generate_auth_token(void)
{
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < AUTH_TOKEN_LEN - 1; i++) {
        config.auth_token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    config.auth_token[AUTH_TOKEN_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Generated auth token: %s", config.auth_token);
}
