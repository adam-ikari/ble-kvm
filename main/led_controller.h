#pragma once

#include <stdbool.h>

typedef enum {
    LED_STATE_PC1_ACTIVE,
    LED_STATE_PC2_ACTIVE,
    LED_STATE_SWITCHING,
    LED_STATE_PAIRING,
    LED_STATE_NO_PC,
    LED_STATE_ERROR,
} led_state_t;

void led_controller_init(void);
void led_controller_set_state(led_state_t state);
