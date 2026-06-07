#include "indicator.h"
#include "board.h"
#if HAS_RGB_LED

#include "esp_log.h"

static const char *TAG = "rgb_led";

void rgb_led_init(void)
{
    ESP_LOGI(TAG, "RGB LED initialized (stub)");
}

void rgb_led_set_state(indicator_state_t state)
{
}

#endif // HAS_RGB_LED
