#include "mic_driver.h"
#include "board.h"

#if HAS_VOICE_INPUT

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"

static const char *TAG = "mic_driver";
static i2s_chan_handle_t rx_handle = NULL;
static bool running = false;

void mic_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MIC_I2S_MCK_GPIO,
            .bclk = MIC_I2S_BCK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = GPIO_NUM_NC,
            .din  = MIC_I2S_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

    ESP_LOGI(TAG, "I2S mic driver initialized on port %d", MIC_I2S_PORT);
}

void mic_driver_start(void)
{
    if (rx_handle && !running) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
        running = true;
        ESP_LOGI(TAG, "Mic capture started");
    }
}

void mic_driver_stop(void)
{
    if (rx_handle && running) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle));
        running = false;
        ESP_LOGI(TAG, "Mic capture stopped");
    }
}

int mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms)
{
    if (!rx_handle || !running) {
        if (bytes_read) *bytes_read = 0;
        return -1;
    }

    esp_err_t err = i2s_channel_read(rx_handle, buf, len, bytes_read, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(err));
        return -1;
    }

    return 0;
}

bool mic_driver_is_running(void)
{
    return running;
}

#endif /* HAS_VOICE_INPUT */
