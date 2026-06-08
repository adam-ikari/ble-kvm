#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef void (*usb_host_keyboard_cb_t)(const uint8_t *report, uint8_t len);
typedef void (*usb_host_mouse_cb_t)(const uint8_t *report, uint8_t len);

void usb_host_init(usb_host_keyboard_cb_t kb_cb, usb_host_mouse_cb_t ms_cb);
bool usb_host_is_keyboard_connected(void);
bool usb_host_is_mouse_connected(void);
