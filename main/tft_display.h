#pragma once
#include "board.h"
#if HAS_TFT_DISPLAY
#include "driver/i2c_master.h"
#include "esp_lcd_types.h"
esp_lcd_panel_handle_t tft_display_get_panel(void);
void tft_display_freeze(bool freeze);
void tft_display_toggle_page(void);
i2c_master_bus_handle_t tft_display_get_i2c_bus(void);
#endif
