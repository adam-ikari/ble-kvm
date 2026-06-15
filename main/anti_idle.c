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
static portMUX_TYPE anti_idle_spinlock = portMUX_INITIALIZER_UNLOCKED;
static int64_t pc_last_activity[3] = {0, 0, 0};  /* per-PC last activity timestamps (microseconds) */

static void send_mouse_nudge_to_pc(uint8_t pc_id)
{
    const kvm_config_t *cfg = config_get();

    if (pc_id == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        if (!usb_device_is_connected()) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        usb_device_send_mouse(report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        usb_device_send_mouse(report2, sizeof(report2));
#endif
    } else {
        uint16_t conn = ble_peripheral_get_conn_handle(pc_id - 1);
        if (conn == 0 || conn == 0xFFFF) return;
        uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report1, sizeof(report1));
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
        ble_peripheral_send_hid_report(conn, 2, report2, sizeof(report2));
    }
}

static void send_mouse_nudge(void)
{
    if (!config_get()->anti_idle_enabled) return;
    int64_t now = esp_timer_get_time();
    uint64_t interval_us = (uint64_t)config_get()->anti_idle_interval_sec * 1000000;

    for (uint8_t pc = 1; pc <= 3; pc++) {
        int64_t last = pc_last_activity[pc - 1];
        if (last == 0) continue;  /* PC never had activity, skip */
        if ((now - last) < (int64_t)interval_us) continue;  /* Recently active, skip */
        send_mouse_nudge_to_pc(pc);
        pc_last_activity[pc - 1] = now;  /* Reset after nudge */
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
    portENTER_CRITICAL(&anti_idle_spinlock);
    if (cfg->anti_idle_enabled && has_input) {
        uint16_t interval = cfg->anti_idle_interval_sec;
        esp_timer_start_periodic(idle_timer, (uint64_t)interval * 1000000);
        active = true;
    } else {
        active = false;
    }
    portEXIT_CRITICAL(&anti_idle_spinlock);
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
    int64_t now = esp_timer_get_time();
    bool is_active;
    portENTER_CRITICAL(&anti_idle_spinlock);
    is_active = active;
    /* Mark all connected PCs as active */
    for (int i = 0; i < 3; i++) {
        pc_last_activity[i] = now;
    }
    portEXIT_CRITICAL(&anti_idle_spinlock);
    if (is_active) {
        restart_timer();
    }
}

void anti_idle_on_pc_connected(uint8_t pc_id)
{
    if (pc_id < 1 || pc_id > 3) return;
    portENTER_CRITICAL(&anti_idle_spinlock);
    pc_last_activity[pc_id - 1] = esp_timer_get_time();
    portEXIT_CRITICAL(&anti_idle_spinlock);
}

void anti_idle_set_enabled(bool enabled)
{
    config_update_bool(CONFIG_FIELD_ANTI_IDLE_ENABLED, enabled);
    restart_timer();
}

void anti_idle_set_interval(uint16_t interval_sec)
{
    if (interval_sec < 10) interval_sec = 10;
    if (interval_sec > 3600) interval_sec = 3600;
    config_update_u16(CONFIG_FIELD_ANTI_IDLE_INTERVAL, interval_sec);
    restart_timer();
}
