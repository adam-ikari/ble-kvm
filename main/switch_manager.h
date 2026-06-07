#pragma once

#include <stdint.h>

void switch_manager_init(void);
void switch_manager_request_switch(void);
uint8_t switch_manager_get_active_pc(void);
uint16_t switch_manager_get_active_conn_handle(void);
void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle);
void switch_manager_on_pc_disconnected(uint8_t pc_id);
