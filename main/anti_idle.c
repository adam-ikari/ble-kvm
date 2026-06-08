#include "anti_idle.h"
#include "config_manager.h"
#include "switch_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#if HAS_USB
#include "usb_device.h"
#include "usb_host.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "anti_idle";
static esp_timer_handle_t idle_timer;
static bool active = false;

static void send_mouse_nudge(void)
{
    const kvm_config_t *cfg = config_get();

    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        if (!usb_device_is_connected()) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        usb_device_send_mouse(report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        usb_device_send_mouse(report2, sizeof(report2));
#endif
    } else {
        uint16_t conn = switch_manager_get_active_conn_handle();
        if (conn == 0 || conn == 0xFFFF) return;
        if (!ble_central_is_mouse_connected()) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report2, sizeof(report2));
    }
}

static void idle_timer_cb(void *arg)
{
    if (!config_get()->anti_idle_enabled) return;
    send_mouse_nudge();
}

static void restart_timer(void)
{
    esp_timer_stop(idle_timer);
    const kvm_config_t *cfg = config_get();
    bool has_input = false;
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_HOST) {
        has_input = usb_host_is_mouse_connected();
    } else {
        has_input = ble_central_is_mouse_connected();
    }
#else
    has_input = ble_central_is_mouse_connected();
#endif
    if (cfg->anti_idle_enabled && has_input) {
        uint16_t interval = cfg->anti_idle_interval_sec;
        esp_timer_start_periodic(idle_timer, (uint64_t)interval * 1000000);
        active = true;
    } else {
        active = false;
    }
}

void anti_idle_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = idle_timer_cb,
        .name = "anti_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &idle_timer));

    if (config_get()->anti_idle_enabled) {
        restart_timer();
    }
    ESP_LOGI(TAG, "Anti-idle initialized (enabled=%d, interval=%ds)",
             config_get()->anti_idle_enabled, config_get()->anti_idle_interval_sec);
}

void anti_idle_on_activity(void)
{
    if (active) {
        restart_timer();
    }
}

void anti_idle_set_enabled(bool enabled)
{
    config_get_mutable()->anti_idle_enabled = enabled;
    config_save_anti_idle();
    restart_timer();
}

void anti_idle_set_interval(uint16_t interval_sec)
{
    if (interval_sec < 30) interval_sec = 30;
    if (interval_sec > 3600) interval_sec = 3600;
    config_get_mutable()->anti_idle_interval_sec = interval_sec;
    config_save_anti_idle();
    restart_timer();
}
