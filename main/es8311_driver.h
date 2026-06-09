#pragma once

#include "driver/i2c_master.h"

#if HAS_VOICE_INPUT

void es8311_init(i2c_master_bus_handle_t i2c_bus);
void es8311_set_mic_gain(uint8_t gain);
void es8311_deinit(void);

#else

static inline void es8311_init(i2c_master_bus_handle_t i2c_bus) { (void)i2c_bus; }
static inline void es8311_set_mic_gain(uint8_t gain) { (void)gain; }
static inline void es8311_deinit(void) {}

#endif
