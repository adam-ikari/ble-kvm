#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

typedef struct {
    int16_t x;
    int16_t y;
} imu_cursor_t;

void imu_driver_init(i2c_master_bus_handle_t i2c_bus);
void imu_driver_read_cursor(uint8_t sensitivity, imu_cursor_t *out);
