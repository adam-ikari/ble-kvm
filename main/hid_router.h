#pragma once

#include <stdint.h>

typedef void (*hid_activity_cb_t)(void);

void hid_router_init(void);
void hid_router_forward_keyboard(const uint8_t *report, uint8_t len);
void hid_router_forward_mouse(const uint8_t *report, uint8_t len);
void hid_router_register_activity_cb(hid_activity_cb_t cb);
void hid_router_on_usb_keyboard(const uint8_t *report, uint8_t len);
void hid_router_on_usb_mouse(const uint8_t *report, uint8_t len);
