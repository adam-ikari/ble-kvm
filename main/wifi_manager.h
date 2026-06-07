#pragma once

#include <stdbool.h>

void wifi_manager_init(void);
void wifi_manager_start_ap(void);
void wifi_manager_start_sta(const char *ssid, const char *password);
void wifi_manager_stop(void);
bool wifi_manager_is_connected(void);
char *wifi_manager_get_ip(void);
