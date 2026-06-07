#include "led_controller.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "led";
static const gpio_num_t LED1_GPIO = GPIO_NUM_2;
static const gpio_num_t LED2_GPIO = GPIO_NUM_1;

static led_state_t current_state = LED_STATE_NO_PC;
static TaskHandle_t led_task_handle = NULL;

static void set_leds(bool led1, bool led2)
{
    gpio_set_level(LED1_GPIO, led1 ? 0 : 1);  // active low
    gpio_set_level(LED2_GPIO, led2 ? 0 : 1);
}

static void led_task(void *arg)
{
    bool toggle = false;
    while (1) {
        switch (current_state) {
        case LED_STATE_PC1_ACTIVE:
            set_leds(true, false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case LED_STATE_PC2_ACTIVE:
            set_leds(false, true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case LED_STATE_SWITCHING:
            set_leds(toggle, !toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        case LED_STATE_PAIRING:
            set_leds(toggle, toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_STATE_NO_PC:
            set_leds(toggle, false);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case LED_STATE_ERROR:
            set_leds(toggle, toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

void led_controller_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED1_GPIO) | (1ULL << LED2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    set_leds(false, false);

    xTaskCreate(led_task, "led_ctrl", 1024, NULL, 1, &led_task_handle);
    ESP_LOGI(TAG, "LED controller initialized");
}

void led_controller_set_state(led_state_t state)
{
    current_state = state;
}
