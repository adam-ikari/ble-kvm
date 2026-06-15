#include "input_mode.h"
#if HAS_INPUT_MODES

#include "config_manager.h"
#include "event_bus.h"
#include "ble_peripheral.h"
#include "switch_manager.h"
#include "imu_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "input_mode";
static input_mode_t current_mode = INPUT_MODE_KVM;
static TaskHandle_t air_mouse_task = NULL;
static esp_timer_handle_t consumer_release_timer = NULL;

#define CONSUMER_KEY_PAGE_DOWN  0x004E
#define CONSUMER_KEY_PAGE_UP    0x004B

static void consumer_key_release_cb(void *arg)
{
    uint16_t conn = switch_manager_get_active_conn_handle();
    if (conn) ble_peripheral_send_consumer_key(conn, 0x0000);
}

static void send_consumer_key(uint16_t usage)
{
    uint16_t conn = switch_manager_get_active_conn_handle();
    if (!conn) return;
    ble_peripheral_send_consumer_key(conn, usage);
    esp_timer_start_once(consumer_release_timer, 50000);
}

static void air_mouse_task_func(void *arg)
{
    imu_cursor_t cursor;
    while (1) {
        if (current_mode != INPUT_MODE_PPT_AIR) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const kvm_config_t *cfg = config_get();
        imu_driver_read_cursor(cfg->air_mouse_sensitivity, &cursor);
        if (cursor.x != 0 || cursor.y != 0) {
            uint8_t report[4] = {0x00,
                                 (uint8_t)(cursor.x & 0xFF),
                                 (uint8_t)(cursor.y & 0xFF),
                                 0x00};
            uint16_t conn = switch_manager_get_active_conn_handle();
            if (conn) ble_peripheral_send_hid_report(conn, 2, report, 4);
            /* Notify power manager of IMU activity */
            APP_EVENT_POST(APP_EVENT_INPUT_IMU_MOTION, NULL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void start_air_mouse(void)
{
    if (!air_mouse_task) {
        xTaskCreate(air_mouse_task_func, "air_mouse", 3072, NULL, 2, &air_mouse_task);
    }
}

static void stop_air_mouse(void)
{
    if (air_mouse_task) {
        vTaskDelete(air_mouse_task);
        air_mouse_task = NULL;
    }
}

void input_mode_init(void)
{
    const kvm_config_t *cfg = config_get();
    current_mode = (input_mode_t)cfg->input_mode;

    esp_timer_create_args_t timer_args = {
        .callback = consumer_key_release_cb,
        .name = "cons_release",
    };
    esp_timer_create(&timer_args, &consumer_release_timer);

    /* IMU is already initialized by main.c before input_mode_init() */

    if (current_mode == INPUT_MODE_PPT_AIR) {
        start_air_mouse();
    }

    ESP_LOGI(TAG, "Input mode: %d", current_mode);
}

input_mode_t input_mode_get(void)
{
    return current_mode;
}

void input_mode_set(input_mode_t mode)
{
    if (mode == current_mode) return;
    input_mode_t old = current_mode;
    if (current_mode == INPUT_MODE_PPT_AIR) stop_air_mouse();
    current_mode = mode;
    config_update_u8(CONFIG_FIELD_INPUT_MODE, (uint8_t)mode);
    if (mode == INPUT_MODE_PPT_AIR) start_air_mouse();
    app_evt_input_mode_changed_t evt = { .old_mode = (uint8_t)old, .new_mode = (uint8_t)mode };
    APP_EVENT_POST(APP_EVENT_INPUT_MODE_CHANGED, &evt, sizeof(evt));
    ESP_LOGI(TAG, "Mode set to %d", mode);
}

void input_mode_cycle(void)
{
    input_mode_set(current_mode == INPUT_MODE_KVM ? INPUT_MODE_PPT_AIR : INPUT_MODE_KVM);
}

void input_mode_on_primary_button(void)
{
    if (current_mode == INPUT_MODE_PPT_AIR) {
        send_consumer_key(CONSUMER_KEY_PAGE_DOWN);
    }
}

void input_mode_on_secondary_button(void)
{
    if (current_mode == INPUT_MODE_PPT_AIR) {
        /* Side button in PPT mode: switch back to KVM */
        input_mode_set(INPUT_MODE_KVM);
    }
}

#endif /* HAS_INPUT_MODES */
