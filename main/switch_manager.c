#include "switch_manager.h"
#include "ble_peripheral.h"
#include "led_controller.h"
#include "config_manager.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "switch";
static const gpio_num_t BUTTON_GPIO = GPIO_NUM_0;

typedef struct {
    switch_source_t source;
} switch_request_t;

static QueueHandle_t switch_queue;
static TaskHandle_t switch_task_handle;
static int64_t button_press_time = 0;
static bool button_pending = false;

static void update_led_state(void)
{
    const kvm_config_t *cfg = config_get();
    bool pc1 = ble_peripheral_is_pc_connected(1);
    bool pc2 = ble_peripheral_is_pc_connected(2);

    if (!pc1 && !pc2) {
        led_controller_set_state(LED_STATE_NO_PC);
    } else if (cfg->active_pc == 1 && pc1) {
        led_controller_set_state(LED_STATE_PC1_ACTIVE);
    } else if (cfg->active_pc == 2 && pc2) {
        led_controller_set_state(LED_STATE_PC2_ACTIVE);
    } else if (pc1) {
        led_controller_set_state(LED_STATE_PC1_ACTIVE);
    } else {
        led_controller_set_state(LED_STATE_PC2_ACTIVE);
    }
}

static void switch_task_func(void *arg)
{
    switch_request_t req;
    while (1) {
        if (xQueueReceive(switch_queue, &req, pdMS_TO_TICKS(100))) {
            kvm_config_t *cfg = config_get_mutable();
            uint8_t old_pc = cfg->active_pc;
            uint8_t new_pc = (old_pc == 1) ? 2 : 1;

            if (!ble_peripheral_is_pc_connected(new_pc)) {
                ESP_LOGW(TAG, "PC%d not connected, cannot switch", new_pc);
                update_led_state();
                continue;
            }

            led_controller_set_state(LED_STATE_SWITCHING);
            vTaskDelay(pdMS_TO_TICKS(100));

            cfg->active_pc = new_pc;
            config_save_active_pc();

            ESP_LOGI(TAG, "Switched from PC%d to PC%d (source=%d)", old_pc, new_pc, req.source);
            update_led_state();
        } else {
            update_led_state();
        }
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_GPIO);

    if (level == 0 && !button_pending) {
        button_press_time = now;
        button_pending = true;
    } else if (level == 1 && button_pending) {
        int64_t duration = now - button_press_time;
        button_pending = false;

        if (duration > 50 && duration < 2000) {
            switch_request_t req = { .source = SWITCH_SRC_BUTTON };
            xQueueSendFromISR(switch_queue, &req, NULL);
        }
    }
}

void switch_manager_init(void)
{
    switch_queue = xQueueCreate(4, sizeof(switch_request_t));
    xTaskCreate(switch_task_func, "switch_mgr", 2048, NULL, 3, &switch_task_handle);

    vTaskDelay(pdMS_TO_TICKS(2000));  // 2s boot delay for BOOT button

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    ESP_LOGI(TAG, "Switch manager initialized");
}

void switch_manager_request_switch(switch_source_t source)
{
    switch_request_t req = { .source = source };
    xQueueSend(switch_queue, &req, pdMS_TO_TICKS(100));
}

uint8_t switch_manager_get_active_pc(void)
{
    return config_get()->active_pc;
}

uint16_t switch_manager_get_active_conn_handle(void)
{
    return ble_peripheral_get_conn_handle(config_get()->active_pc);
}

void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle)
{
    ESP_LOGI(TAG, "PC%d connected (handle=%d)", pc_id, conn_handle);
    update_led_state();
}

void switch_manager_on_pc_disconnected(uint8_t pc_id)
{
    ESP_LOGI(TAG, "PC%d disconnected", pc_id);
    kvm_config_t *cfg = config_get_mutable();
    if (cfg->active_pc == pc_id) {
        uint8_t other = (pc_id == 1) ? 2 : 1;
        if (ble_peripheral_is_pc_connected(other)) {
            cfg->active_pc = other;
            config_save_active_pc();
        }
    }
    update_led_state();
}
