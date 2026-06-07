#include "indicator.h"
#include "board.h"
#if HAS_TFT_DISPLAY

#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "config_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "switch_manager.h"
#include "wifi_manager.h"

static const char *TAG = "tft";
static esp_lcd_panel_handle_t panel = NULL;
static indicator_state_t current_state = IND_NO_PC;
static TaskHandle_t tft_task_handle = NULL;
static int current_page = 0;
static int64_t last_button_time = 0;

/* 5x7 bitmap font for 0-9, A-Z, common symbols */
static const uint8_t font5x7[][5] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00}, /* space */
    [0x2D] = {0x00,0x00,0x7F,0x00,0x00}, /* - */
    [0x2E] = {0x00,0x00,0x00,0x60,0x00}, /* . */
    [0x2F] = {0x00,0x01,0x06,0x08,0x00}, /* / */
    [0x30] = {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    [0x31] = {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    [0x32] = {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    [0x33] = {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    [0x34] = {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    [0x35] = {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    [0x36] = {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    [0x37] = {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    [0x38] = {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    [0x39] = {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    [0x3A] = {0x00,0x36,0x36,0x00,0x00}, /* : */
    [0x41] = {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    [0x42] = {0x7F,0x49,0x49,0x49,0x36}, /* B */
    [0x43] = {0x3E,0x41,0x41,0x41,0x22}, /* C */
    [0x44] = {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    [0x45] = {0x7F,0x49,0x49,0x49,0x41}, /* E */
    [0x46] = {0x7F,0x09,0x09,0x09,0x01}, /* F */
    [0x47] = {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    [0x48] = {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    [0x49] = {0x00,0x41,0x7F,0x41,0x00}, /* I */
    [0x4B] = {0x7F,0x08,0x14,0x22,0x41}, /* K */
    [0x4C] = {0x7F,0x40,0x40,0x40,0x40}, /* L */
    [0x4D] = {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    [0x4E] = {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    [0x4F] = {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    [0x50] = {0x7F,0x09,0x09,0x09,0x06}, /* P */
    [0x52] = {0x7F,0x09,0x19,0x29,0x46}, /* R */
    [0x53] = {0x46,0x49,0x49,0x49,0x31}, /* S */
    [0x54] = {0x01,0x01,0x7F,0x01,0x01}, /* T */
    [0x55] = {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    [0x56] = {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    [0x57] = {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    [0x58] = {0x63,0x14,0x08,0x14,0x63}, /* X */
    [0x59] = {0x07,0x08,0x70,0x08,0x07}, /* Y */
    [0x5A] = {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

#define RGB565(r,g,b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
#define COLOR_BG     RGB565(0, 0, 0)
#define COLOR_FG     RGB565(255, 255, 255)
#define COLOR_GREEN  RGB565(0, 255, 0)
#define COLOR_RED    RGB565(255, 0, 0)
#define COLOR_GRAY   RGB565(100, 100, 100)
#define COLOR_BLUE   RGB565(0, 100, 255)

static uint16_t fb[TFT_WIDTH * TFT_HEIGHT];

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int px = x + col, py = y + row;
            if (px >= 0 && px < TFT_WIDTH && py >= 0 && py < TFT_HEIGHT) {
                fb[py * TFT_WIDTH + px] = color;
            }
        }
    }
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x5A) c = ' ';
    const uint8_t *glyph = font5x7[(uint8_t)c];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (bits & (1 << row)) ? fg : bg;
            int px = x + col, py = y + row;
            if (px >= 0 && px < TFT_WIDTH && py >= 0 && py < TFT_HEIGHT) {
                fb[py * TFT_WIDTH + px] = color;
            }
        }
    }
}

static void draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        draw_char(x, y, *s, fg, bg);
        x += 6;
        s++;
    }
}

static void draw_big_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x5A) c = ' ';
    const uint8_t *glyph = font5x7[(uint8_t)c];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = (bits & (1 << row)) ? fg : bg;
            fill_rect(x + col * 3, y + row * 3, 3, 3, color);
        }
    }
}

static void flush_fb(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, TFT_WIDTH, TFT_HEIGHT, fb);
}

static void draw_status_page(void)
{
    memset(fb, 0, sizeof(fb));
    const kvm_config_t *cfg = config_get();

    bool pc1 = ble_peripheral_is_pc_connected(1);
    draw_str(4, 8, "PC1", COLOR_FG, COLOR_BG);
    draw_char(28, 8, pc1 ? 'O' : ' ', pc1 ? COLOR_GREEN : COLOR_RED, COLOR_BG);

    bool pc2 = ble_peripheral_is_pc_connected(2);
    draw_str(4, 20, "PC2", COLOR_FG, COLOR_BG);
    draw_char(28, 20, pc2 ? 'O' : ' ', pc2 ? COLOR_GREEN : COLOR_RED, COLOR_BG);

    char active_str[8];
    snprintf(active_str, sizeof(active_str), "PC%d", cfg->active_pc);
    int ax = (TFT_WIDTH - 5 * 3 * 3) / 2;
    for (int i = 0; active_str[i]; i++) {
        draw_big_char(ax + i * 18, 60, active_str[i], COLOR_GREEN, COLOR_BG);
    }

    bool kb = ble_central_is_keyboard_connected();
    bool ms = ble_central_is_mouse_connected();
    draw_str(4, 140, "KB", kb ? COLOR_GREEN : COLOR_RED, COLOR_BG);
    draw_str(40, 140, "MS", ms ? COLOR_GREEN : COLOR_RED, COLOR_BG);

#if HAS_BATTERY
    draw_str(4, 160, "BAT", COLOR_FG, COLOR_BG);
#endif

    flush_fb();
}

static void draw_debug_page(void)
{
    memset(fb, 0, sizeof(fb));
    const kvm_config_t *cfg = config_get();

    draw_str(4, 8, "WiFi", COLOR_FG, COLOR_BG);
    wifi_operating_mode_t mode = wifi_manager_get_mode();
    const char *mode_str = "OFF";
    if (mode == KVM_WIFI_AP_ONLY) mode_str = "AP";
    else if (mode == KVM_WIFI_STA_ONLY) mode_str = "STA";
    else if (mode == KVM_WIFI_APSTA) mode_str = "AP+STA";
    draw_str(40, 8, mode_str, COLOR_FG, COLOR_BG);

    draw_str(4, 20, wifi_manager_get_sta_ip(), COLOR_FG, COLOR_BG);

    draw_str(4, 40, "SSID:", COLOR_FG, COLOR_BG);
    draw_str(4, 52, cfg->wifi_ssid[0] ? cfg->wifi_ssid : "-", COLOR_FG, COLOR_BG);

    int ble_conns = 0;
    if (ble_central_is_keyboard_connected()) ble_conns++;
    if (ble_central_is_mouse_connected()) ble_conns++;
    if (ble_peripheral_is_pc_connected(1)) ble_conns++;
    if (ble_peripheral_is_pc_connected(2)) ble_conns++;
    char ble_str[24];
    snprintf(ble_str, sizeof(ble_str), "BLE:%d conn", ble_conns);
    draw_str(4, 80, ble_str, COLOR_FG, COLOR_BG);

    flush_fb();
}

static void tft_task(void *arg)
{
    while (1) {
        if (current_page == 0) {
            draw_status_page();
        } else {
            draw_debug_page();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void tft_display_toggle_page(void)
{
    current_page = (current_page + 1) % 2;
    last_button_time = esp_timer_get_time() / 1000;
}

static void init_backlight(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num = TFT_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 128,
    };
    ledc_channel_config(&ch_cfg);
}

static i2c_master_bus_handle_t i2c_bus;

static void init_pmic_power(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev));

    uint8_t reg_val = 0x00;
    uint8_t read_cmd = 0x16;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, &read_cmd, 1, &reg_val, 1, -1));
    reg_val |= 0x04;
    uint8_t write_buf[] = {0x16, reg_val};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, write_buf, sizeof(write_buf), -1));

    vTaskDelay(pdMS_TO_TICKS(100));
}

i2c_master_bus_handle_t tft_display_get_i2c_bus(void)
{
    return i2c_bus;
}

void tft_display_init(void)
{
    init_pmic_power();

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = TFT_SCLK_GPIO,
        .mosi_io_num = TFT_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TFT_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = TFT_DC_GPIO,
        .cs_gpio_num = TFT_CS_GPIO,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, TFT_OFFSET_X, TFT_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    init_backlight();

    memset(fb, 0, sizeof(fb));
    flush_fb();

    xTaskCreate(tft_task, "tft_disp", 4096, NULL, 1, &tft_task_handle);
    ESP_LOGI(TAG, "TFT display initialized");
}

void tft_display_set_state(indicator_state_t state)
{
    current_state = state;
}

#endif // HAS_TFT_DISPLAY