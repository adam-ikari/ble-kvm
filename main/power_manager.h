#pragma once

#include <stdbool.h>
#include <stdint.h>

void power_manager_init(void);
bool power_manager_is_charging(void);
uint8_t power_manager_get_battery_percent(void);
uint16_t power_manager_get_battery_voltage_mv(void);
bool power_manager_is_usb_powered(void);
