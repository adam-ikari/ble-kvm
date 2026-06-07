#include "power_manager.h"
#include "board.h"
#if HAS_BATTERY

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "power";
static i2c_master_dev_handle_t pmic_dev;
static int64_t last_activity_time = 0;

#define SLEEP_IDLE_MS      (5 * 60 * 1000)
#define SLEEP_DEEP_MS      (15 * 60 * 1000)

static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(pmic_dev, &reg, 1, val, 1, -1);
}

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[] = {reg, val};
    return i2c_master_transmit(pmic_dev, buf, sizeof(buf), -1);
}

void power_manager_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    extern i2c_master_bus_handle_t tft_display_get_i2c_bus(void);
    bus = tft_display_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &pmic_dev));

    last_activity_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Power manager initialized");
}

bool power_manager_is_charging(void)
{
    uint8_t val = 0;
    if (pmic_read(0x01, &val) != ESP_OK) return false;
    return (val & 0x01) != 0;
}

uint8_t power_manager_get_battery_percent(void)
{
    uint16_t mv = power_manager_get_battery_voltage_mv();
    if (mv == 0) return 0;
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    return (uint8_t)((mv - 3000) * 100 / 1200);
}

uint16_t power_manager_get_battery_voltage_mv(void)
{
    uint8_t val = 0;
    if (pmic_read(0x34, &val) != ESP_OK) return 0;
    return (uint16_t)(val * 17);
}

bool power_manager_is_usb_powered(void)
{
    uint8_t val = 0;
    if (pmic_read(0x00, &val) != ESP_OK) return true;
    return (val & 0x80) != 0;
}

#endif // HAS_BATTERY
