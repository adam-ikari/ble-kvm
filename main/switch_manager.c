#include "switch_manager.h"
#include "ble_peripheral.h"
#include "indicator.h"
#include "board.h"
#include "config_manager.h"
#include "input_mode.h"
#if HAS_USB
#include "usb_device.h"
#endif
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_system.h"

static const char *TAG = "switch";

#define CMD_SWITCH       1
#define CMD_SECONDARY    2
#define CMD_FACTORY_RST  3
#define CMD_MODE_CYCLE   4

#define LONG_PRESS_MS 5000

static bool is_pc3_connected(void)
{
#if HAS_USB
    return config_get()->usb_mode == USB_MODE_DEVICE && usb_device_is_connected();
#else
    return false;
#endif
}

static QueueHandle_t switch_queue;
static TaskHandle_t switch_task_handle;
static int64_t button_press_time = 0;
static bool button_pending = false;
static bool long_press_triggered = false;

static void update_led_state(void)
{
    const kvm_config_t *cfg = config_get();
    bool pc1 = ble_peripheral_is_pc_connected(1);
    bool pc2 = ble_peripheral_is_pc_connected(2);
    bool pc3 = is_pc3_connected();

    if (!pc1 && !pc2 && !pc3) {
        indicator_set_state(IND_NO_PC);
    } else if (cfg->active_pc == 1 && pc1) {
        indicator_set_state(IND_PC1_ACTIVE);
    } else if (cfg->active_pc == 2 && pc2) {
        indicator_set_state(IND_PC2_ACTIVE);
    } else if (cfg->active_pc == 3 && pc3) {
        indicator_set_state(IND_PC3_ACTIVE);
    } else if (pc1) {
        indicator_set_state(IND_PC1_ACTIVE);
    } else if (pc2) {
        indicator_set_state(IND_PC2_ACTIVE);
    } else {
        indicator_set_state(IND_PC3_ACTIVE);
    }
}

static void do_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset triggered!");
    indicator_set_state(IND_PAIRING);

    nvs_flash_erase();
    esp_restart();
}

static void switch_task_func(void *arg)
{
    uint8_t cmd;
    while (1) {
        if (xQueueReceive(switch_queue, &cmd, pdMS_TO_TICKS(100))) {
            if (cmd == CMD_SWITCH) {
                if (input_mode_get() != INPUT_MODE_KVM) {
                    input_mode_on_primary_button();
                } else {
                    kvm_config_t *cfg = config_get_mutable();
                    uint8_t old_pc = cfg->active_pc;
                    uint8_t new_pc = old_pc;

                    /* Cycle through connected PCs: 1 -> 2 -> 3 -> 1 */
                    for (int attempt = 0; attempt < 3; attempt++) {
                        new_pc = (new_pc % 3) + 1;
                        if (new_pc == 3) {
                            if (is_pc3_connected()) break;
                        } else {
                            if (ble_peripheral_is_pc_connected(new_pc)) break;
                        }
                    }
                    if (new_pc == old_pc) {
                        ESP_LOGW(TAG, "No other connected PC to switch to");
                        update_led_state();
                        continue;
                    }

                    indicator_set_state(IND_PAIRING);
                    vTaskDelay(pdMS_TO_TICKS(100));

                    cfg->active_pc = new_pc;
                    config_save_active_pc();

                    ESP_LOGI(TAG, "Switched from PC%d to PC%d", old_pc, new_pc);
                    update_led_state();
                }
            }
#if HAS_SECONDARY_BUTTON
            else if (cmd == CMD_SECONDARY) {
                if (input_mode_get() != INPUT_MODE_KVM) {
                    input_mode_on_secondary_button();
                } else {
                    extern void tft_display_toggle_page(void);
                    tft_display_toggle_page();
                }
            }
#endif
            else if (cmd == CMD_FACTORY_RST) {
                do_factory_reset();
            }
            else if (cmd == CMD_MODE_CYCLE) {
                input_mode_cycle();
            }
        } else {
            update_led_state();
        }
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SWITCH_GPIO);

    if (level == 0 && !button_pending) {
        button_press_time = now;
        button_pending = true;
        long_press_triggered = false;
    } else if (level == 1 && button_pending) {
        int64_t duration = now - button_press_time;
        button_pending = false;

        if (duration >= LONG_PRESS_MS) {
            uint8_t cmd = CMD_FACTORY_RST;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (duration > 50) {
            uint8_t cmd = CMD_SWITCH;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    }
}

#if HAS_SECONDARY_BUTTON
static void IRAM_ATTR secondary_button_isr_handler(void *arg)
{
    static int64_t sec_press_time = 0;
    static bool sec_pending = false;
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SECONDARY_GPIO);

    if (level == 0 && !sec_pending) {
        sec_press_time = now;
        sec_pending = true;
    } else if (level == 1 && sec_pending) {
        int64_t duration = now - sec_press_time;
        sec_pending = false;
        if (duration > 50 && duration < 1000) {
            uint8_t cmd = CMD_SECONDARY;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (duration >= 1000 && duration < 3000) {
            uint8_t cmd = CMD_MODE_CYCLE;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    }
}
#endif

void switch_manager_init(void)
{
    switch_queue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(switch_task_func, "switch_mgr", 2048, NULL, 3, &switch_task_handle);

    vTaskDelay(pdMS_TO_TICKS(2000));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_SWITCH_GPIO, button_isr_handler, NULL);

#if HAS_SECONDARY_BUTTON
    gpio_config_t io2 = {
        .pin_bit_mask = (1ULL << BUTTON_SECONDARY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io2);
    gpio_isr_handler_add(BUTTON_SECONDARY_GPIO, secondary_button_isr_handler, NULL);
#endif

    ESP_LOGI(TAG, "Switch manager initialized");
}

void switch_manager_request_switch(void)
{
    uint8_t cmd = CMD_SWITCH;
    xQueueSend(switch_queue, &cmd, pdMS_TO_TICKS(100));
}

uint8_t switch_manager_get_active_pc(void)
{
    return config_get()->active_pc;
}

uint16_t switch_manager_get_active_conn_handle(void)
{
    const kvm_config_t *cfg = config_get();
    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
        return 0xFFFF;
    }
    return ble_peripheral_get_conn_handle(cfg->active_pc);
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
        for (uint8_t candidate = 1; candidate <= 3; candidate++) {
            if (candidate == pc_id) continue;
            if (candidate == 3) {
                if (is_pc3_connected()) {
                    cfg->active_pc = candidate;
                    config_save_active_pc();
                    break;
                }
            } else {
                if (ble_peripheral_is_pc_connected(candidate)) {
                    cfg->active_pc = candidate;
                    config_save_active_pc();
                    break;
                }
            }
        }
    }
    update_led_state();
}
