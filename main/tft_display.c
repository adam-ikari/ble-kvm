#include "indicator.h"
#include "board.h"
#if HAS_TFT_DISPLAY

#include "esp_log.h"

static const char *TAG = "tft_display";

void tft_display_init(void)
{
    ESP_LOGI(TAG, "TFT display initialized (stub)");
}

void tft_display_set_state(indicator_state_t state)
{
}

#endif // HAS_TFT_DISPLAY
