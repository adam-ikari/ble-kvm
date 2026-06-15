#pragma once

#include <stdint.h>

void switch_manager_init(void);
void switch_manager_request_switch(void);
uint8_t switch_manager_get_active_pc(void);
uint16_t switch_manager_get_active_conn_handle(void);
