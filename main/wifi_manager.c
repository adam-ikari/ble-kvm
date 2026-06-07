#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "config_manager.h"
#include <string.h>

static const char *TAG = "wifi";

static wifi_operating_mode_t current_mode = KVM_WIFI_OFF;
static bool sta_connected = false;
static bool ap_active = false;
static char sta_ip[16] = "0.0.0.0";
static char ap_ssid[33] = {0};
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static bool wifi_initialized = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "AP: client disconnected");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "STA: disconnected, reconnecting...");
            sta_connected = false;
            if (current_mode == KVM_WIFI_STA_ONLY || current_mode == KVM_WIFI_APSTA) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA: connected to AP");
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA: got ip %s", sta_ip);
            sta_connected = true;
            break;
        }
        case IP_EVENT_AP_STAIPASSIGNED: {
            ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
            ESP_LOGI(TAG, "AP: assigned ip " IPSTR, IP2STR(&event->ip));
            break;
        }
        default:
            break;
        }
    }
}

static void ensure_wifi_initialized(void)
{
    if (wifi_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi driver initialized");
}

static void apply_mode(void)
{
    wifi_mode_t mode;
    switch (current_mode) {
    case KVM_WIFI_AP_ONLY:
        mode = WIFI_MODE_AP;
        break;
    case KVM_WIFI_STA_ONLY:
        mode = WIFI_MODE_STA;
        break;
    case KVM_WIFI_APSTA:
        mode = WIFI_MODE_APSTA;
        break;
    case KVM_WIFI_OFF:
    default:
        esp_wifi_stop();
        ESP_LOGI(TAG, "Wi-Fi stopped");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (current_mode == KVM_WIFI_STA_ONLY || current_mode == KVM_WIFI_APSTA) {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "Wi-Fi mode set to %d", current_mode);
}

void wifi_manager_init(void)
{
    ensure_wifi_initialized();

    // Always start AP mode for initial access
    wifi_manager_start_ap();

    // If STA credentials saved, also connect to router
    const kvm_config_t *config = config_get();
    if (config->wifi_ssid[0] != '\0' && config->wifi_enabled) {
        wifi_manager_start_sta(config->wifi_ssid, config->wifi_password);
    }

    ESP_LOGI(TAG, "Wi-Fi manager initialized, mode=%d", current_mode);
}

void wifi_manager_set_mode(wifi_operating_mode_t mode)
{
    if (mode == current_mode) return;

    ESP_LOGI(TAG, "Switching Wi-Fi mode: %d -> %d", current_mode, mode);

    // Stop current connections
    if (current_mode != KVM_WIFI_OFF) {
        if (sta_connected) {
            esp_wifi_disconnect();
            sta_connected = false;
        }
        esp_wifi_stop();
    }

    current_mode = mode;

    // Apply new mode
    switch (mode) {
    case KVM_WIFI_AP_ONLY:
        ap_active = true;
        sta_connected = false;
        apply_mode();
        break;
    case KVM_WIFI_STA_ONLY:
        ap_active = false;
        {
            const kvm_config_t *config = config_get();
            if (config->wifi_ssid[0] != '\0') {
                wifi_config_t sta_config = {0};
                strncpy((char *)sta_config.sta.ssid, config->wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
                strncpy((char *)sta_config.sta.password, config->wifi_password, sizeof(sta_config.sta.password) - 1);
                esp_wifi_set_config(WIFI_IF_STA, &sta_config);
            }
        }
        apply_mode();
        break;
    case KVM_WIFI_APSTA:
        ap_active = true;
        {
            const kvm_config_t *config = config_get();
            if (config->wifi_ssid[0] != '\0') {
                wifi_config_t sta_config = {0};
                strncpy((char *)sta_config.sta.ssid, config->wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
                strncpy((char *)sta_config.sta.password, config->wifi_password, sizeof(sta_config.sta.password) - 1);
                esp_wifi_set_config(WIFI_IF_STA, &sta_config);
            }
        }
        apply_mode();
        break;
    case KVM_WIFI_OFF:
        ap_active = false;
        sta_connected = false;
        apply_mode();
        break;
    }
}

wifi_operating_mode_t wifi_manager_get_mode(void)
{
    return current_mode;
}

void wifi_manager_start_ap(void)
{
    wifi_config_t ap_config = {0};
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
             "BLE-KVM-%02X%02X", mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    strncpy(ap_ssid, (char *)ap_config.ap.ssid, sizeof(ap_ssid) - 1);

    if (current_mode == KVM_WIFI_OFF || current_mode == KVM_WIFI_STA_ONLY) {
        current_mode = (current_mode == KVM_WIFI_STA_ONLY) ? KVM_WIFI_APSTA : KVM_WIFI_AP_ONLY;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(
        (current_mode == KVM_WIFI_APSTA) ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ap_active = true;

    ESP_LOGI(TAG, "AP started: SSID=%s", ap_config.ap.ssid);
}

void wifi_manager_stop_ap(void)
{
    if (!ap_active) return;

    if (current_mode == KVM_WIFI_APSTA) {
        current_mode = KVM_WIFI_STA_ONLY;
    } else if (current_mode == KVM_WIFI_AP_ONLY) {
        current_mode = KVM_WIFI_OFF;
    }

    ap_active = false;

    if (current_mode == KVM_WIFI_OFF) {
        esp_wifi_stop();
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        // STA will auto-reconnect via event handler
    }

    ESP_LOGI(TAG, "AP stopped");
}

void wifi_manager_start_sta(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    // Save to config
    kvm_config_t *cfg = config_get_mutable();
    strncpy(cfg->wifi_ssid, ssid, sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_password, password, sizeof(cfg->wifi_password) - 1);
    cfg->wifi_enabled = true;
    config_save_wifi();

    if (current_mode == KVM_WIFI_OFF || current_mode == KVM_WIFI_AP_ONLY) {
        current_mode = (current_mode == KVM_WIFI_AP_ONLY) ? KVM_WIFI_APSTA : KVM_WIFI_STA_ONLY;
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    if (ap_active) {
        mode = WIFI_MODE_APSTA;
    }

    esp_wifi_set_mode(mode);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "STA connecting to %s", ssid);
}

void wifi_manager_stop_sta(void)
{
    if (!sta_connected && current_mode != KVM_WIFI_STA_ONLY && current_mode != KVM_WIFI_APSTA) {
        return;
    }

    esp_wifi_disconnect();
    sta_connected = false;

    if (current_mode == KVM_WIFI_APSTA) {
        current_mode = KVM_WIFI_AP_ONLY;
        esp_wifi_set_mode(WIFI_MODE_AP);
    } else if (current_mode == KVM_WIFI_STA_ONLY) {
        current_mode = KVM_WIFI_OFF;
        esp_wifi_stop();
    }

    // Save to config
    kvm_config_t *cfg = config_get_mutable();
    cfg->wifi_enabled = false;
    config_save_wifi();

    ESP_LOGI(TAG, "STA stopped");
}

void wifi_manager_stop(void)
{
    esp_wifi_stop();
    current_mode = KVM_WIFI_OFF;
    ap_active = false;
    sta_connected = false;
    ESP_LOGI(TAG, "Wi-Fi stopped");
}

bool wifi_manager_is_sta_connected(void)
{
    return sta_connected;
}

bool wifi_manager_is_ap_active(void)
{
    return ap_active;
}

char *wifi_manager_get_sta_ip(void)
{
    return sta_connected ? sta_ip : (char *)"0.0.0.0";
}

char *wifi_manager_get_ap_ip(void)
{
    return (char *)"192.168.4.1";
}

char *wifi_manager_get_ap_ssid(void)
{
    return ap_ssid;
}

// Legacy compat
bool wifi_manager_is_connected(void) __attribute__((alias("wifi_manager_is_sta_connected")));
char *wifi_manager_get_ip(void) __attribute__((alias("wifi_manager_get_sta_ip")));
