#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "led_controller.h"
#include "config_manager.h"
#include "ble_peripheral.h"

static const char *TAG = "main";

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    ble_peripheral_start_advertising();
}

void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    config_manager_init();
    led_controller_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_peripheral_init();
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE-KVM initialized, token: %s", config_get()->auth_token);
}
