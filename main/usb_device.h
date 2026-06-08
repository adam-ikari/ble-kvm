#pragma once

#include <stdint.h>
#include <stdbool.h>

void usb_device_init(void);
bool usb_device_is_connected(void);
int usb_device_send_keyboard(const uint8_t *report, uint8_t len);
int usb_device_send_mouse(const uint8_t *report, uint8_t len);
