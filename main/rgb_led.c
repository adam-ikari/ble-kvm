#include "indicator.h"
#include "board.h"
#if HAS_RGB_LED

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_led";

#define WS2812B_T0H_NS   300
#define WS2812B_T0L_NS   900
#define WS2812B_T1H_NS   900
#define WS2812B_T1L_NS   300

static rmt_channel_handle_t tx_chan;
static rmt_encoder_handle_t encoder;
static indicator_state_t current_state = IND_NO_PC;
static TaskHandle_t rgb_task_handle = NULL;

static void send_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = {g, r, b};
    rmt_symbol_word_t symbols[24];
    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            int idx = i * 8 + (7 - bit);
            if (grb[i] & (1 << bit)) {
                symbols[idx].duration0 = WS2812B_T1H_NS / 25;
                symbols[idx].level0 = 1;
                symbols[idx].duration1 = WS2812B_T1L_NS / 25;
                symbols[idx].level1 = 0;
            } else {
                symbols[idx].duration0 = WS2812B_T0H_NS / 25;
                symbols[idx].level0 = 1;
                symbols[idx].duration1 = WS2812B_T0L_NS / 25;
                symbols[idx].level1 = 0;
            }
        }
    }
    rmt_transmit_config_t config = {.flags = 0};
    rmt_transmit(tx_chan, encoder, symbols, sizeof(symbols), &config);
}

static void rgb_task(void *arg)
{
    bool toggle = false;
    while (1) {
        switch (current_state) {
        case IND_PC1_ACTIVE:
            send_rgb(0, 255, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PC2_ACTIVE:
            send_rgb(0, 0, 255);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PC3_ACTIVE:
            send_rgb(180, 0, 255);  /* purple for USB PC */
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_NO_PC:
            send_rgb(toggle ? 255 : 0, 0, 0);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PAIRING:
            send_rgb(toggle ? 255 : 0, toggle ? 255 : 0, toggle ? 255 : 0);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

void rgb_led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &tx_chan));
    ESP_ERROR_CHECK(rmt_enable(tx_chan));

    rmt_bytes_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &encoder));

    xTaskCreate(rgb_task, "rgb_led", 2048, NULL, 1, &rgb_task_handle);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", RGB_LED_GPIO);
}

void rgb_led_set_state(indicator_state_t state)
{
    current_state = state;
}

#endif // HAS_RGB_LED
