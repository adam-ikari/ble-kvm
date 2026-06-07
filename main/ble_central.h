#pragma once

#include <stdint.h>
#include <stdbool.h>

void ble_central_init(void);
void ble_central_start_scan(void);
void ble_central_stop_scan(void);
void ble_central_connect_keyboard(const uint8_t *addr, uint8_t addr_type);
void ble_central_connect_mouse(const uint8_t *addr, uint8_t addr_type);
bool ble_central_is_keyboard_connected(void);
bool ble_central_is_mouse_connected(void);
const char *ble_central_get_scan_results_json(void);
