#pragma once

#include <stdint.h>
#include <stdbool.h>

void usb_host_init(void);
bool usb_host_is_keyboard_connected(void);
bool usb_host_is_mouse_connected(void);
