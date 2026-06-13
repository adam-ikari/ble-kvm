#include "switch_manager.h"
#include "ble_peripheral.h"
#include "indicator.h"
#include "board.h"
#include "config_manager.h"
#include "input_mode.h"
#include "web_server.h"
#if HAS_BATTERY
#include "power_manager.h"
#endif
#if HAS_USB
#include "usb_device.h"
#endif
#if HAS_VOICE_INPUT
#include "voice_input.h"
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

#define CMD_SWITCH         1
#define CMD_SECONDARY      2
#define CMD_FACTORY_RST    3
#define CMD_MODE_CYCLE     4
#define CMD_VOICE_START    5
#define CMD_VOICE_STOP     6
#define CMD_FACTORY_WARN   7
#define CMD_FACTORY_CANCEL 8
#define CMD_WEB_AUTH       9
#define CMD_PPT_PAGE_UP    10

#define VOICE_PRESS_MS     500
#define MODE_CYCLE_MS      5000
#define FACTORY_WARN_MS    5000
#define FACTORY_RST_MS     10000

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
static portMUX_TYPE switch_spinlock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t button_press_time = 0;
static volatile bool button_pending = false;
static volatile bool long_press_triggered = false;
static volatile bool factory_warned = false;

static esp_timer_handle_t voice_start_timer;
static esp_timer_handle_t dc_timeout_timer;  /* double-click window timeout */

#if HAS_SECONDARY_BUTTON
static volatile int64_t sec_press_time = 0;
static volatile bool sec_pending = false;
static volatile bool sec_factory_warned = false;
static esp_timer_handle_t factory_warn_timer;
#endif

static void update_led_state(void)
{
    const kvm_config_t *cfg = config_get();
    /* ble_peripheral uses 0-indexed slots (0=PC1, 1=PC2) */
    bool pc1 = ble_peripheral_is_pc_connected(0);
    bool pc2 = ble_peripheral_is_pc_connected(1);
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
#if HAS_BATTERY
            pm_sleep_on_activity();
#endif
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
                            /* ble_peripheral uses 0-indexed slots */
                            if (ble_peripheral_is_pc_connected(new_pc - 1)) break;
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
                    web_server_notify_switch(new_pc);
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
            else if (cmd == CMD_VOICE_START) {
#if HAS_VOICE_INPUT
                if (!voice_input_is_active()) {
                    if (!voice_input_start()) {
                        ESP_LOGW(TAG, "Voice start failed (need WiFi/config)");
                    }
                }
#endif
            }
            else if (cmd == CMD_VOICE_STOP) {
#if HAS_VOICE_INPUT
                if (voice_input_is_active()) {
                    voice_input_stop();
                }
#endif
            }
            else if (cmd == CMD_FACTORY_WARN) {
                ESP_LOGW(TAG, "Factory reset warning — hold 10s to confirm");
                indicator_set_state(IND_PAIRING);
            }
            else if (cmd == CMD_FACTORY_CANCEL) {
                ESP_LOGI(TAG, "Factory reset cancelled");
                update_led_state();
            }
            else if (cmd == CMD_WEB_AUTH) {
                extern void web_server_grant_auth(void);
                web_server_grant_auth();
                ESP_LOGI(TAG, "Web auth granted via double-click");
            }
            else if (cmd == CMD_PPT_PAGE_UP) {
#if HAS_INPUT_MODES
                uint16_t conn = switch_manager_get_active_conn_handle();
                if (conn != 0xFFFF && conn != 0) {
                    ble_peripheral_send_consumer_key(conn, 0x004B); /* Page Up */
                }
#endif
            }
        } else {
            update_led_state();
        }
    }
}

static void voice_start_timer_cb(void *arg)
{
    /* This runs in timer task context — safe to call esp_timer_stop.
     * The ISR only sets button_pending/press_time; we check elapsed time here. */
    portENTER_CRITICAL(&switch_spinlock);
    if (!button_pending) {
        portEXIT_CRITICAL(&switch_spinlock);
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    int64_t duration = now - button_press_time;

#if HAS_SECONDARY_BUTTON
    /* StickS3: long press = voice start */
    if (duration >= VOICE_PRESS_MS && !long_press_triggered) {
        long_press_triggered = true;
        portEXIT_CRITICAL(&switch_spinlock);
        uint8_t cmd = CMD_VOICE_START;
        xQueueSend(switch_queue, &cmd, 0);
    } else {
        portEXIT_CRITICAL(&switch_spinlock);
    }
#else
    /* Single-button: factory reset at 10s, warning at 5s */
    if (duration >= FACTORY_RST_MS && !long_press_triggered) {
        long_press_triggered = true;
        portEXIT_CRITICAL(&switch_spinlock);
        uint8_t cmd = CMD_FACTORY_RST;
        xQueueSend(switch_queue, &cmd, 0);
    } else if (duration >= FACTORY_WARN_MS && !factory_warned) {
        factory_warned = true;
        portEXIT_CRITICAL(&switch_spinlock);
        uint8_t cmd = CMD_FACTORY_WARN;
        xQueueSend(switch_queue, &cmd, 0);
    } else {
        portEXIT_CRITICAL(&switch_spinlock);
    }
#endif
}

static volatile int64_t dc_last_release_time = 0;
static volatile bool dc_waiting_second = false;

/* One-shot timer: fires after double-click window expires, dispatches CMD_SWITCH */
static void dc_timeout_timer_cb(void *arg)
{
    if (dc_waiting_second) {
        dc_waiting_second = false;
        uint8_t cmd = CMD_SWITCH;
        xQueueSend(switch_queue, &cmd, 0);
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SWITCH_GPIO);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (level == 0 && !button_pending) {
        button_press_time = now;
        button_pending = true;
        long_press_triggered = false;
        /* Timer runs continuously; callback checks button_pending */
    } else if (level == 1 && button_pending) {
        button_pending = false;

        if (long_press_triggered) {
#if HAS_SECONDARY_BUTTON
            uint8_t cmd = CMD_VOICE_STOP;
            xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
#else
            /* Single-button: factory reset already queued in timer callback.
             * Nothing to do on release — the reset will execute. */
#endif
        } else {
#if !HAS_SECONDARY_BUTTON
            /* Single-button: if warned but released before reset, cancel */
            if (factory_warned) {
                factory_warned = false;
                uint8_t cmd = CMD_FACTORY_CANCEL;
                xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
            }
#endif
            int64_t duration = now - button_press_time;
            if (duration > 50) {
                int64_t since_last = now - dc_last_release_time;
                dc_last_release_time = now;

                if (dc_waiting_second && since_last < 500) {
                    /* Double-click: web auth (KVM) or Page Up (PPT) */
                    dc_waiting_second = false;
                    esp_timer_stop(dc_timeout_timer);
#if HAS_INPUT_MODES
                    if (input_mode_get() == INPUT_MODE_PPT_AIR) {
                        uint8_t cmd = CMD_PPT_PAGE_UP;
                        xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
                    } else
#endif
                    {
                        uint8_t cmd = CMD_WEB_AUTH;
                        xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
                    }
                } else {
                    /* First click: arm double-click window.
                     * CMD_SWITCH is deferred — only dispatched after
                     * 500ms if no second click arrives. */
                    dc_waiting_second = true;
                    esp_timer_start_once(dc_timeout_timer, 500000);  /* 500ms */
                }
            }
        }
    }

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

#if HAS_SECONDARY_BUTTON
static void factory_warn_timer_cb(void *arg)
{
    /* This runs in timer task context — safe to call esp_timer_stop.
     * The ISR only sets sec_pending/sec_press_time; we check elapsed time here. */
    portENTER_CRITICAL(&switch_spinlock);
    if (!sec_pending) {
        portEXIT_CRITICAL(&switch_spinlock);
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    int64_t duration = now - sec_press_time;

    if (duration >= FACTORY_RST_MS) {
        sec_pending = false;
        portEXIT_CRITICAL(&switch_spinlock);
        uint8_t cmd = CMD_FACTORY_RST;
        xQueueSend(switch_queue, &cmd, 0);
    } else if (duration >= FACTORY_WARN_MS && !sec_factory_warned) {
        sec_factory_warned = true;
        portEXIT_CRITICAL(&switch_spinlock);
        uint8_t cmd = CMD_FACTORY_WARN;
        xQueueSend(switch_queue, &cmd, 0);
    } else {
        portEXIT_CRITICAL(&switch_spinlock);
    }
}

static void IRAM_ATTR secondary_button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SECONDARY_GPIO);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (level == 0 && !sec_pending) {
        sec_press_time = now;
        sec_pending = true;
        sec_factory_warned = false;
        /* Timer runs continuously; callback checks sec_pending */
    } else if (level == 1 && sec_pending) {
        int64_t duration = now - sec_press_time;
        sec_pending = false;

        if (sec_factory_warned) {
            uint8_t cmd = CMD_FACTORY_CANCEL;
            xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
        } else if (duration > 50 && duration < 1000) {
            uint8_t cmd = CMD_SECONDARY;
            xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
        } else if (duration >= 1000 && duration < MODE_CYCLE_MS) {
            uint8_t cmd = CMD_MODE_CYCLE;
            xQueueSendFromISR(switch_queue, &cmd, &xHigherPriorityTaskWoken);
        }
    }

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#endif

void switch_manager_init(void)
{
    switch_queue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(switch_task_func, "switch_mgr", 4096, NULL, 3, &switch_task_handle);

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

    const esp_timer_create_args_t voice_timer_args = {
        .name = "voice_start",
        .callback = voice_start_timer_cb,
    };
    esp_timer_create(&voice_timer_args, &voice_start_timer);
    /* Timer runs continuously — ISR sets button_pending, callback checks it */
    esp_timer_start_periodic(voice_start_timer, 50000);  /* 50ms */

#if HAS_SECONDARY_BUTTON
    const esp_timer_create_args_t factory_timer_args = {
        .name = "factory_warn",
        .callback = factory_warn_timer_cb,
    };
    esp_timer_create(&factory_timer_args, &factory_warn_timer);
    /* Timer runs continuously — ISR sets sec_pending, callback checks it */
    esp_timer_start_periodic(factory_warn_timer, 200000);  /* 200ms */
#endif

    const esp_timer_create_args_t dc_timer_args = {
        .name = "dc_timeout",
        .callback = dc_timeout_timer_cb,
    };
    esp_timer_create(&dc_timer_args, &dc_timeout_timer);

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
    return ble_peripheral_get_conn_handle(cfg->active_pc - 1);  /* 0-indexed */
}

void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle)
{
    ESP_LOGI(TAG, "PC%d connected (handle=%d)", pc_id, conn_handle);
    update_led_state();
    web_server_notify_connection(pc_id, true);
#if HAS_BATTERY
    pm_sleep_on_pc_connected(pc_id);
#endif
}

void switch_manager_on_pc_disconnected(uint8_t pc_id)
{
    ESP_LOGI(TAG, "PC%d disconnected", pc_id);
    web_server_notify_connection(pc_id, false);
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
                if (ble_peripheral_is_pc_connected(candidate - 1)) {  /* 0-indexed */
                    cfg->active_pc = candidate;
                    config_save_active_pc();
                    break;
                }
            }
        }
    }
    update_led_state();
#if HAS_BATTERY
    pm_sleep_on_pc_disconnected(pc_id);
#endif
}
