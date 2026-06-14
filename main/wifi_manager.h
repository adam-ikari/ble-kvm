#pragma once

#include <stdbool.h>

typedef enum {
    KVM_WIFI_AP_ONLY,       // 仅 AP 模式（热点）
    KVM_WIFI_STA_ONLY,      // 仅 STA 模式（连接路由器）
    KVM_WIFI_APSTA,         // AP+STA 共存模式
    KVM_WIFI_OFF,           // Wi-Fi 关闭
} wifi_operating_mode_t;

void wifi_manager_init(void);
void wifi_manager_set_mode(wifi_operating_mode_t mode);
wifi_operating_mode_t wifi_manager_get_mode(void);
void wifi_manager_start_ap(void);
void wifi_manager_stop_ap(void);
void wifi_manager_start_sta(const char *ssid, const char *password);
void wifi_manager_stop_sta(void);
void wifi_manager_stop(void);
bool wifi_manager_is_sta_connected(void);
bool wifi_manager_is_ap_active(void);
char *wifi_manager_get_sta_ip(void);
char *wifi_manager_get_ap_ip(void);
char *wifi_manager_get_ap_ssid(void);
bool wifi_manager_is_netif_ready(void);
