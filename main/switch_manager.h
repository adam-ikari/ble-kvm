#pragma once

#include <stdint.h>

typedef enum {
    SWITCH_SRC_BUTTON,
    SWITCH_SRC_WEB,
} switch_source_t;

void switch_manager_init(void);
void switch_manager_request_switch(switch_source_t source);
uint8_t switch_manager_get_active_pc(void);
uint16_t switch_manager_get_active_conn_handle(void);
void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle);
void switch_manager_on_pc_disconnected(uint8_t pc_id);
