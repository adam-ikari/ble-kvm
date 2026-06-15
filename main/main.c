#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "indicator.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "switch_manager.h"
#include "hid_router.h"
#include "anti_idle.h"
#include "board.h"
#if HAS_BATTERY || HAS_INPUT_MODES || HAS_VOICE_INPUT
#include "driver/i2c_master.h"
#endif
#if HAS_INPUT_MODES
#include "input_mode.h"
#include "imu_driver.h"
#endif
#if HAS_USB
#include "usb_device.h"
#include "usb_host.h"
#include "hid_router.h"
#endif
#if HAS_BATTERY
#include "power_manager.h"
#endif
#include "web_server.h"
#include "web_log.h"
#if HAS_VOICE_INPUT
#include "es8311_driver.h"
#include "mic_driver.h"
#include "voice_input.h"
#endif

static const char *TAG = "main";

static void on_pc_conn_event(uint8_t pc_id, uint16_t conn_handle, bool connected)
{
    if (connected) {
        switch_manager_on_pc_connected(pc_id, conn_handle);
    } else {
        switch_manager_on_pc_disconnected(pc_id);
    }
}

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
    web_log_init();

    /* Step 1: Init TFT first (creates I2C bus for PMIC) */
    indicator_init();

    /* Step 2: Init I2C peripherals using the shared bus */
#if HAS_BATTERY || HAS_INPUT_MODES || HAS_VOICE_INPUT
    extern i2c_master_bus_handle_t tft_display_get_i2c_bus(void);
    i2c_master_bus_handle_t i2c_bus = tft_display_get_i2c_bus();
#endif

#if HAS_BATTERY
    power_manager_init(i2c_bus);
    pm_sleep_init();
#endif

#if HAS_INPUT_MODES
    imu_driver_init(i2c_bus);
    input_mode_init();
#endif

    const kvm_config_t *cfg = config_get();

#if HAS_VOICE_INPUT
    if (cfg->voice_asr_enabled && cfg->voice_asr_appid != 0) {
        es8311_init(i2c_bus);
        mic_driver_init();
        voice_input_init();
        ESP_LOGI(TAG, "Voice input initialized");
    }
#endif

    /* Step 3: WiFi — web_server_init registers a ready callback,
     * actual httpd_start is deferred until WIFI_EVENT_AP_START */
    web_server_init();
    wifi_manager_init();

    /* Step 4: USB */
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_DEVICE) {
        usb_device_init();
        ESP_LOGI(TAG, "USB Device mode active");
    } else if (cfg->usb_mode == USB_MODE_HOST) {
        usb_host_init();
        ESP_LOGI(TAG, "USB Host mode active");
    } else {
        ESP_LOGI(TAG, "USB disabled (BLE-only)");
    }
#endif

    /* Step 5: BLE */
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_peripheral_init();
    if (cfg->usb_mode != USB_MODE_HOST) {
        ble_central_init();
    }

    /* Start NimBLE host task BEFORE switch_manager_init() so the
     * auto-connect timer (2s, created in ble_central_init) doesn't
     * fire before the NimBLE event loop is active. */
    nimble_port_freertos_init(ble_host_task);

    /* Step 6: Input & routing */
    switch_manager_init();
    hid_router_init();
    anti_idle_init();
    ble_peripheral_register_conn_cb(on_pc_conn_event);

    /* Step 7: Done — web server starts via WIFI_EVENT_AP_START callback */
    ESP_LOGI(TAG, "BLE-KVM initialized, usb_mode: %d",
             cfg->usb_mode);
}
