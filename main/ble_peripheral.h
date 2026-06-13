#pragma once

#include <stdint.h>
#include "config_manager.h"

void ble_peripheral_init(void);
void ble_peripheral_start_advertising(void);
void ble_peripheral_start_directed_advertising(const pc_device_t *pc);
void ble_peripheral_stop_advertising(void);
uint16_t ble_peripheral_get_conn_handle(uint8_t pc_id);
int ble_peripheral_send_hid_report(uint16_t conn_handle, uint8_t report_id,
                                    const uint8_t *data, uint8_t len);
int ble_peripheral_send_consumer_key(uint16_t conn_handle, uint16_t usage_code);
bool ble_peripheral_is_pc_connected(uint8_t pc_id);

void ble_peripheral_enter_pairing_mode(void);
void ble_peripheral_exit_pairing_mode(void);
bool ble_peripheral_is_pairing_mode(void);

typedef void (*ble_peripheral_conn_cb_t)(uint8_t pc_id, uint16_t conn_handle, bool connected);
void ble_peripheral_register_conn_cb(ble_peripheral_conn_cb_t cb);
