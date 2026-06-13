#pragma once
#include "board.h"
#if HAS_TFT_DISPLAY
#include "esp_lcd_panel.h"
esp_lcd_panel_handle_t tft_display_get_panel(void);
void tft_display_freeze(bool freeze);
#endif
