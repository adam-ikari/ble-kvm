#pragma once

#include <stdint.h>
#include <stdbool.h>

void web_server_init(void);
void web_server_grant_auth(void);
void web_server_notify_switch(uint8_t active_pc);
void web_server_notify_connection(uint8_t pc_id, bool connected);
void web_server_notify_device(const char *device_type, bool connected);
