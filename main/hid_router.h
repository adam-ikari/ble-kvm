#pragma once

#include <stdint.h>

void hid_router_init(void);
void hid_router_forward_keyboard(const uint8_t *report, uint8_t len);
void hid_router_forward_mouse(const uint8_t *report, uint8_t len);
void hid_router_on_usb_keyboard(const uint8_t *report, uint8_t len);
void hid_router_on_usb_mouse(const uint8_t *report, uint8_t len);
