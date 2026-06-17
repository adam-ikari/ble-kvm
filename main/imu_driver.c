#include "imu_driver.h"
#include "board.h"
#if HAS_INPUT_MODES

#include "esp_log.h"
#include "event_bus.h"
#include <math.h>

static const char *TAG = "imu";
static i2c_master_dev_handle_t imu_dev = NULL;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define REG_PWR_MGMT_1   0x6B
#define REG_ACCEL_XOUT_H 0x3B

void imu_driver_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &imu_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add IMU device: %s", esp_err_to_name(err));
        return;
    }

    /* Wake up: write 0x00 to PWR_MGMT_1 */
    uint8_t wake[] = {REG_PWR_MGMT_1, 0x00};
    err = i2c_master_transmit(imu_dev, wake, sizeof(wake), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake IMU: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "IMU initialized at addr 0x%02X", IMU_I2C_ADDR);
}

void imu_driver_read_cursor(uint8_t sensitivity, imu_cursor_t *out)
{
    out->x = 0;
    out->y = 0;
#if HAS_BATTERY
    APP_EVENT_POST(APP_EVENT_HID_ACTIVITY, NULL, 0);
#endif
    if (!imu_dev) return;

    uint8_t reg = REG_ACCEL_XOUT_H;
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit_receive(imu_dev, &reg, 1, buf, 6, 100);
    if (err != ESP_OK) return;

    int16_t ax = (buf[0] << 8) | buf[1];
    int16_t ay = (buf[2] << 8) | buf[3];
    int16_t az = (buf[4] << 8) | buf[5];

    float pitch = atan2f((float)ax, sqrtf((float)ay * ay + (float)az * az)) * 180.0f / M_PI;
    float roll  = atan2f((float)ay, sqrtf((float)ax * ax + (float)az * az)) * 180.0f / M_PI;

    #define DEAD_ZONE 2.0f
    if (fabsf(pitch) > DEAD_ZONE) {
        out->y = (int16_t)(pitch * sensitivity / 5.0f);
        if (out->y > 127) out->y = 127;
        if (out->y < -127) out->y = -127;
    }
    if (fabsf(roll) > DEAD_ZONE) {
        out->x = (int16_t)(roll * sensitivity / 5.0f);
        if (out->x > 127) out->x = 127;
        if (out->x < -127) out->x = -127;
    }
}

#endif /* HAS_INPUT_MODES */
