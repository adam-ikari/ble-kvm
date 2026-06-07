#include "indicator.h"
#include "board.h"

#if HAS_GPIO_LED
void gpio_led_init(void);
void gpio_led_set_state(indicator_state_t state);
#endif

#if HAS_RGB_LED
void rgb_led_init(void);
void rgb_led_set_state(indicator_state_t state);
#endif

#if HAS_TFT_DISPLAY
void tft_display_init(void);
void tft_display_set_state(indicator_state_t state);
#endif

void indicator_init(void)
{
#if HAS_GPIO_LED
    gpio_led_init();
#endif
#if HAS_RGB_LED
    rgb_led_init();
#endif
#if HAS_TFT_DISPLAY
    tft_display_init();
#endif
}

void indicator_set_state(indicator_state_t state)
{
#if HAS_GPIO_LED
    gpio_led_set_state(state);
#endif
#if HAS_RGB_LED
    rgb_led_set_state(state);
#endif
#if HAS_TFT_DISPLAY
    tft_display_set_state(state);
#endif
}
