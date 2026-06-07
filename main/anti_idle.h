#pragma once

#include <stdbool.h>
#include <stdint.h>

void anti_idle_init(void);
void anti_idle_on_activity(void);
void anti_idle_set_enabled(bool enabled);
void anti_idle_set_interval(uint16_t interval_sec);
