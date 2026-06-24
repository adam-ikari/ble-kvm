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
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "config_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "wifi_manager.h"
#include "input_mode.h"
#if HAS_USB
#include "usb_device.h"
#include "usb_host.h"
#endif

static const char *TAG = "tft";
static esp_lcd_panel_handle_t panel = NULL;
static indicator_state_t current_state = IND_NO_PC;
static int current_page = 0;
static int64_t warn_state_enter_time = 0;
static portMUX_TYPE tft_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── 5×7 font (printable ASCII 0x20–0x5A) ──────────────────────────── */
static const uint8_t font5x7[][5] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00}, [0x21] = {0x00,0x00,0x5F,0x00,0x00},
    [0x22] = {0x00,0x07,0x00,0x07,0x00}, [0x23] = {0x14,0x7F,0x14,0x7F,0x14},
    [0x24] = {0x24,0x2A,0x7F,0x2A,0x12}, [0x25] = {0x23,0x13,0x08,0x64,0x62},
    [0x26] = {0x36,0x49,0x55,0x22,0x50}, [0x27] = {0x00,0x05,0x03,0x00,0x00},
    [0x28] = {0x00,0x1C,0x22,0x41,0x00}, [0x29] = {0x00,0x41,0x22,0x1C,0x00},
    [0x2A] = {0x08,0x2A,0x1C,0x2A,0x08}, [0x2B] = {0x08,0x08,0x3E,0x08,0x08},
    [0x2C] = {0x00,0x50,0x30,0x00,0x00}, [0x2D] = {0x08,0x08,0x08,0x08,0x08},
    [0x2E] = {0x00,0x60,0x60,0x00,0x00}, [0x2F] = {0x20,0x10,0x08,0x04,0x02},
    [0x30] = {0x3E,0x51,0x49,0x45,0x3E}, [0x31] = {0x00,0x42,0x7F,0x40,0x00},
    [0x32] = {0x42,0x61,0x51,0x49,0x46}, [0x33] = {0x21,0x41,0x45,0x4B,0x31},
    [0x34] = {0x18,0x14,0x12,0x7F,0x10}, [0x35] = {0x27,0x45,0x45,0x45,0x39},
    [0x36] = {0x3C,0x4A,0x49,0x49,0x30}, [0x37] = {0x01,0x71,0x09,0x05,0x03},
    [0x38] = {0x36,0x49,0x49,0x49,0x36}, [0x39] = {0x06,0x49,0x49,0x29,0x1E},
    [0x3A] = {0x00,0x36,0x36,0x00,0x00}, [0x3B] = {0x00,0x56,0x36,0x00,0x00},
    [0x3C] = {0x00,0x08,0x14,0x22,0x41}, [0x3D] = {0x14,0x14,0x14,0x14,0x14},
    [0x3E] = {0x41,0x22,0x14,0x08,0x00}, [0x3F] = {0x02,0x01,0x51,0x09,0x06},
    [0x40] = {0x32,0x49,0x79,0x41,0x3E}, [0x41] = {0x7E,0x11,0x11,0x11,0x7E},
    [0x42] = {0x7F,0x49,0x49,0x49,0x36}, [0x43] = {0x3E,0x41,0x41,0x41,0x22},
    [0x44] = {0x7F,0x41,0x41,0x22,0x1C}, [0x45] = {0x7F,0x49,0x49,0x49,0x41},
    [0x46] = {0x7F,0x09,0x09,0x09,0x01}, [0x47] = {0x3E,0x41,0x49,0x49,0x7A},
    [0x48] = {0x7F,0x08,0x08,0x08,0x7F}, [0x49] = {0x00,0x41,0x7F,0x41,0x00},
    [0x4A] = {0x20,0x40,0x41,0x3F,0x01}, [0x4B] = {0x7F,0x08,0x14,0x22,0x41},
    [0x4C] = {0x7F,0x40,0x40,0x40,0x40}, [0x4D] = {0x7F,0x02,0x0C,0x02,0x7F},
    [0x4E] = {0x7F,0x04,0x08,0x10,0x7F}, [0x4F] = {0x3E,0x41,0x41,0x41,0x3E},
    [0x50] = {0x7F,0x09,0x09,0x09,0x06}, [0x51] = {0x3E,0x41,0x51,0x21,0x5E},
    [0x52] = {0x7F,0x09,0x19,0x29,0x46}, [0x53] = {0x46,0x49,0x49,0x49,0x31},
    [0x54] = {0x01,0x01,0x7F,0x01,0x01}, [0x55] = {0x3F,0x40,0x40,0x40,0x3F},
    [0x56] = {0x1F,0x20,0x40,0x20,0x1F}, [0x57] = {0x3F,0x40,0x38,0x40,0x3F},
    [0x58] = {0x63,0x14,0x08,0x14,0x63}, [0x59] = {0x07,0x08,0x70,0x08,0x07},
    [0x5A] = {0x61,0x51,0x49,0x45,0x43},
};

/* ── Color palette ─────────────────────────────────────────────────── */
#define RGB565(r,g,b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
#define C_BG        RGB565(8, 8, 14)
#define C_WHITE     RGB565(230, 230, 240)
#define C_GREEN     RGB565(0, 220, 100)
#define C_RED       RGB565(255, 55, 55)
#define C_GRAY      RGB565(100, 100, 110)
#define C_BLUE      RGB565(60, 150, 255)
#define C_YELLOW    RGB565(255, 210, 0)
#define C_ORANGE    RGB565(255, 140, 0)
#define C_DARK      RGB565(22, 22, 30)
#define C_DARKER    RGB565(14, 14, 20)
#define C_CARD      RGB565(18, 18, 26)
#define C_HEADER    RGB565(16, 16, 24)
#define C_DIVIDER   RGB565(32, 32, 42)
#define C_RED_BG    RGB565(100, 0, 0)
#define C_DIM_WHITE RGB565(160, 160, 170)

/* Framebuffer in PSRAM (64,800 bytes). SPI-DMA cannot read PSRAM directly,
 * so flush_fb() copies line-by-line through a small internal DMA buffer. */
static EXT_RAM_BSS_ATTR uint16_t fb[TFT_WIDTH * TFT_HEIGHT];

/* ── Drawing primitives ────────────────────────────────────────────── */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint16_t *line = &fb[(y + row) * TFT_WIDTH + x];
        for (int col = 0; col < w; col++) line[col] = color;
    }
}

static void draw_hline(int x, int y, int w, uint16_t color)
{
    if (y < 0 || y >= TFT_HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > TFT_WIDTH) w = TFT_WIDTH - x;
    if (w <= 0) return;
    uint16_t *line = &fb[y * TFT_WIDTH + x];
    for (int i = 0; i < w; i++) line[i] = color;
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x5A) c = ' ';
    const uint8_t *glyph = font5x7[(uint8_t)c];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            int px = x + col, py = y + row;
            if (px >= 0 && px < TFT_WIDTH && py >= 0 && py < TFT_HEIGHT)
                fb[py * TFT_WIDTH + px] = (bits & (1 << row)) ? fg : bg;
        }
    }
}

static void draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) { draw_char(x, y, *s++, fg, bg); x += 6; }
}

static void draw_centered(int y, const char *s, uint16_t fg, uint16_t bg)
{
    int x = (TFT_WIDTH - (int)strlen(s) * 6) / 2;
    if (x < 0) x = 0;
    draw_str(x, y, s, fg, bg);
}

static void draw_big_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x5A) c = ' ';
    const uint8_t *glyph = font5x7[(uint8_t)c];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++)
            fill_rect(x + col * 3, y + row * 3, 3, 3, (bits & (1 << row)) ? fg : bg);
    }
}

static void draw_big_str(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) { draw_big_char(x, y, *s++, fg, bg); x += 16; }
}

static void draw_battery(int x, int y, int w, int h, int pct)
{
    /* Body outline */
    draw_hline(x, y, w, C_WHITE);
    draw_hline(x, y + h - 1, w, C_WHITE);
    for (int i = 0; i < h; i++) {
        fb[(y + i) * TFT_WIDTH + x] = C_WHITE;
        fb[(y + i) * TFT_WIDTH + x + w - 1] = C_WHITE;
    }
    /* Terminal cap */
    fill_rect(x + w, y + 2, 2, h - 4, C_WHITE);
    /* Fill level */
    int fill = (w - 2) * pct / 100;
    uint16_t bat_color = pct > 20 ? C_GREEN : (pct > 10 ? C_YELLOW : C_RED);
    fill_rect(x + 1, y + 1, fill, h - 2, bat_color);
}

static void draw_card(int x, int y, int w, int h)
{
    fill_rect(x, y, w, h, C_CARD);
    /* Top accent line */
    draw_hline(x, y, w, C_DIVIDER);
}

/* ── Flush framebuffer ─────────────────────────────────────────────── */
/* PSRAM framebuffer is not directly DMA-capable for SPI. Copy each line
 * through a small (480-byte) internal DMA buffer, then send via
 * esp_lcd_panel_draw_bitmap. This avoids the 64KB internal DMA allocation
 * that would exhaust precious internal DRAM. */
static void flush_fb(void)
{
    /* One line = 240 pixels × 2 bytes = 480 bytes. Small enough to fit
     * comfortably in internal DMA memory. */
    uint16_t *line_buf = heap_caps_malloc(TFT_WIDTH * 2,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line_buf) {
        ESP_LOGE(TAG, "flush_fb: failed to alloc line buffer");
        return;
    }
    for (int y = 0; y < TFT_HEIGHT; y++) {
        memcpy(line_buf, &fb[y * TFT_WIDTH], TFT_WIDTH * 2);
        esp_lcd_panel_draw_bitmap(panel, 0, y, TFT_WIDTH, y + 1, line_buf);
    }
    free(line_buf);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Page 0: Status Dashboard
 * ═══════════════════════════════════════════════════════════════════════ */
static void draw_status_page(void)
{
    memset(fb, 0, sizeof(fb));
    const kvm_config_t *cfg = config_get();

    /* ── Header bar ────────────────────────────────────────────────── */
    fill_rect(0, 0, TFT_WIDTH, 14, C_HEADER);
    draw_centered(3, "BLE-KVM", C_BLUE, C_HEADER);

    /* ── Active PC card ────────────────────────────────────────────── */
    int pc = cfg->active_pc;
    bool pc_ok;
#if HAS_USB
    pc_ok = (pc == 3)
        ? ((cfg->usb_mode == USB_MODE_DEVICE) && usb_device_is_connected())
        : ble_peripheral_is_pc_connected(pc - 1);
#else
    pc_ok = ble_peripheral_is_pc_connected(pc - 1);
#endif

    int card_y = 16, card_h = 48;
    uint16_t card_bg = pc_ok ? RGB565(0, 35, 60) : RGB565(30, 30, 36);
    uint16_t card_fg = pc_ok ? C_GREEN : C_GRAY;
    fill_rect(4, card_y, TFT_WIDTH - 8, card_h, card_bg);
    /* Accent bar on left */
    fill_rect(4, card_y, 3, card_h, pc_ok ? C_GREEN : C_GRAY);

    char s[16];
    snprintf(s, sizeof(s), "PC %d", pc);
    draw_big_str(14, card_y + 4, s, C_WHITE, card_bg);
    draw_str(14, card_y + 30, pc_ok ? "CONNECTED" : "DISCONNECTED", card_fg, card_bg);

    /* Connection type badge */
    const char *ctype = (pc == 3) ? "USB" : "BLE";
    draw_str(TFT_WIDTH - 36, card_y + 4, ctype, C_BLUE, card_bg);

    /* ── PC status row ─────────────────────────────────────────────── */
    int y = card_y + card_h + 4;
    bool c[3] = {
        ble_peripheral_is_pc_connected(0),
        ble_peripheral_is_pc_connected(1),
#if HAS_USB
        (cfg->usb_mode == USB_MODE_DEVICE) && usb_device_is_connected()
#else
        false
#endif
    };

    for (int i = 0; i < 3; i++) {
        int cx = 4 + i * 43;
        uint16_t pc_bg = (i + 1 == pc) ? RGB565(0, 25, 50) : C_CARD;
        uint16_t pc_fg = c[i] ? C_GREEN : C_GRAY;
        fill_rect(cx, y, 41, 22, pc_bg);
        snprintf(s, sizeof(s), "PC%d", i + 1);
        draw_str(cx + 4, y + 4, s, C_WHITE, pc_bg);
        draw_str(cx + 4, y + 13, c[i] ? "ON" : "--", pc_fg, pc_bg);
        /* Active indicator dot */
        if (i + 1 == pc)
            fill_rect(cx + 34, y + 2, 5, 5, pc_ok ? C_GREEN : C_RED);
    }
    y += 26;

    /* ── Divider ───────────────────────────────────────────────────── */
    draw_hline(4, y, TFT_WIDTH - 8, C_DIVIDER);
    y += 4;

    /* ── Peripherals section ───────────────────────────────────────── */
    bool kb = false, ms = false;
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_HOST) {
        kb = usb_host_is_keyboard_connected();
        ms = usb_host_is_mouse_connected();
    } else {
        kb = ble_central_is_keyboard_connected();
        ms = ble_central_is_mouse_connected();
    }
#else
    kb = ble_central_is_keyboard_connected();
    ms = ble_central_is_mouse_connected();
#endif

    /* KB row */
    draw_str(6, y, "KB", C_DIM_WHITE, C_BG);
    fill_rect(22, y + 2, 40, 7, C_DARKER);
    fill_rect(22, y + 2, kb ? 40 : 0, 7, kb ? C_GREEN : C_DARKER);
    draw_str(66, y, kb ? "OK" : "--", kb ? C_GREEN : C_GRAY, C_BG);
    y += 12;

    /* MS row */
    draw_str(6, y, "MS", C_DIM_WHITE, C_BG);
    fill_rect(22, y + 2, 40, 7, C_DARKER);
    fill_rect(22, y + 2, ms ? 40 : 0, 7, ms ? C_GREEN : C_DARKER);
    draw_str(66, y, ms ? "OK" : "--", ms ? C_GREEN : C_GRAY, C_BG);
    y += 12;

    /* Input mode */
    input_mode_t im = input_mode_get();
    draw_str(6, y, "MODE", C_DIM_WHITE, C_BG);
    draw_str(36, y, im == INPUT_MODE_KVM ? "KVM" : "PPT",
             im == INPUT_MODE_KVM ? C_GREEN : C_YELLOW, C_BG);
    y += 14;

    /* ── Divider ───────────────────────────────────────────────────── */
    draw_hline(4, y, TFT_WIDTH - 8, C_DIVIDER);
    y += 4;

    /* ── WiFi section ──────────────────────────────────────────────── */
    wifi_operating_mode_t wm = wifi_manager_get_mode();
    bool ap = (wm == KVM_WIFI_AP_ONLY || wm == KVM_WIFI_APSTA);
    bool sta = (wm == KVM_WIFI_STA_ONLY || wm == KVM_WIFI_APSTA);

    draw_str(6, y, "WiFi", C_DIM_WHITE, C_BG);
    if (ap) {
        draw_str(30, y, "AP", C_GREEN, C_BG);
        draw_str(48, y, wifi_manager_get_ap_ip(), C_BLUE, C_BG);
    } else if (sta) {
        draw_str(30, y, "STA", C_GREEN, C_BG);
        draw_str(54, y, wifi_manager_get_sta_ip(), C_BLUE, C_BG);
    } else {
        draw_str(30, y, "OFF", C_GRAY, C_BG);
    }
    y += 14;

    /* ── Battery ───────────────────────────────────────────────────── */
#if HAS_BATTERY
    draw_hline(4, y, TFT_WIDTH - 8, C_DIVIDER);
    y += 4;
    extern uint8_t power_manager_get_battery_percent(void);
    extern bool power_manager_is_charging(void);
    int bat = power_manager_get_battery_percent();
    draw_str(6, y, "BAT", C_DIM_WHITE, C_BG);
    draw_battery(30, y, 44, 9, bat);
    snprintf(s, sizeof(s), "%d%%", bat);
    draw_str(78, y, s, C_WHITE, C_BG);
    if (power_manager_is_charging())
        draw_str(102, y, "CHG", C_YELLOW, C_BG);
    y += 14;
#endif

    /* ── Footer: button hints ──────────────────────────────────────── */
    fill_rect(0, TFT_HEIGHT - 12, TFT_WIDTH, 12, C_HEADER);
    draw_hline(0, TFT_HEIGHT - 12, TFT_WIDTH, C_DIVIDER);
    draw_str(4, TFT_HEIGHT - 9, "1:Switch", C_GRAY, C_HEADER);
    draw_str(54, TFT_HEIGHT - 9, "2x:Web", C_GRAY, C_HEADER);
#if HAS_SECONDARY_BUTTON
    draw_str(96, TFT_HEIGHT - 9, "2:Mode", C_GRAY, C_HEADER);
#endif

    flush_fb();
}

/* ═══════════════════════════════════════════════════════════════════════
 * Page 1: System Info
 * ═══════════════════════════════════════════════════════════════════════ */
static void draw_debug_page(void)
{
    memset(fb, 0, sizeof(fb));
    const kvm_config_t *cfg = config_get();

    /* Header */
    fill_rect(0, 0, TFT_WIDTH, 14, C_HEADER);
    draw_centered(3, "SYSTEM INFO", C_DIM_WHITE, C_HEADER);

    int y = 18, lh = 14;
    char buf[40];

    /* ── BLE section ───────────────────────────────────────────────── */
    draw_str(6, y, "BLE", C_BLUE, C_BG);
    y += lh;

    int ble_conns = 0;
    if (ble_central_is_keyboard_connected()) ble_conns++;
    if (ble_central_is_mouse_connected()) ble_conns++;
    if (ble_peripheral_is_pc_connected(0)) ble_conns++;
    if (ble_peripheral_is_pc_connected(1)) ble_conns++;

    snprintf(buf, sizeof(buf), "  Connections: %d", ble_conns);
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh;

    snprintf(buf, sizeof(buf), "  KB: %s  MS: %s",
             ble_central_is_keyboard_connected() ? "OK" : "--",
             ble_central_is_mouse_connected() ? "OK" : "--");
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh;

    snprintf(buf, sizeof(buf), "  PC1: %s  PC2: %s",
             ble_peripheral_is_pc_connected(0) ? "ON" : "--",
             ble_peripheral_is_pc_connected(1) ? "ON" : "--");
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh + 2;

    /* ── WiFi section ──────────────────────────────────────────────── */
    draw_hline(6, y, TFT_WIDTH - 12, C_DIVIDER); y += 4;
    draw_str(6, y, "WiFi", C_BLUE, C_BG); y += lh;

    wifi_operating_mode_t wm = wifi_manager_get_mode();
    const char *mode_str = (wm == KVM_WIFI_AP_ONLY) ? "AP" :
                           (wm == KVM_WIFI_STA_ONLY) ? "STA" :
                           (wm == KVM_WIFI_APSTA) ? "AP+STA" : "OFF";
    snprintf(buf, sizeof(buf), "  Mode: %s", mode_str);
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh;

    if (wm == KVM_WIFI_AP_ONLY || wm == KVM_WIFI_APSTA) {
        snprintf(buf, sizeof(buf), "  AP IP: %s", wifi_manager_get_ap_ip());
        draw_str(6, y, buf, C_BLUE, C_BG); y += lh;
    }
    if (wm == KVM_WIFI_STA_ONLY || wm == KVM_WIFI_APSTA) {
        snprintf(buf, sizeof(buf), "  STA IP: %s", wifi_manager_get_sta_ip());
        draw_str(6, y, buf, C_BLUE, C_BG); y += lh;
    }
    y += 2;

    /* ── USB section ───────────────────────────────────────────────── */
    draw_hline(6, y, TFT_WIDTH - 12, C_DIVIDER); y += 4;
    draw_str(6, y, "USB", C_BLUE, C_BG); y += lh;

#if HAS_USB
    const char *usb_modes[] = {"Disabled", "Device (HID)", "Host"};
    snprintf(buf, sizeof(buf), "  Mode: %s", usb_modes[cfg->usb_mode]);
#else
    snprintf(buf, sizeof(buf), "  Mode: N/A");
#endif
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh + 2;

    /* ── Sensors section ───────────────────────────────────────────── */
    draw_hline(6, y, TFT_WIDTH - 12, C_DIVIDER); y += 4;
    draw_str(6, y, "Sensors", C_BLUE, C_BG); y += lh;

    draw_str(6, y, "  IMU: OK", C_GREEN, C_BG); y += lh;

    input_mode_t im = input_mode_get();
    snprintf(buf, sizeof(buf), "  Input: %s", im == INPUT_MODE_KVM ? "KVM" : "PPT/Air Mouse");
    draw_str(6, y, buf, im == INPUT_MODE_KVM ? C_GREEN : C_YELLOW, C_BG); y += lh;

    snprintf(buf, sizeof(buf), "  Sensitivity: %d/10", cfg->air_mouse_sensitivity);
    draw_str(6, y, buf, C_WHITE, C_BG); y += lh + 2;

    /* ── Firmware ──────────────────────────────────────────────────── */
    draw_hline(6, y, TFT_WIDTH - 12, C_DIVIDER); y += 4;
    draw_str(6, y, "Firmware", C_BLUE, C_BG); y += lh;
    draw_str(6, y, "  BLE-KVM v1.0", C_DIM_WHITE, C_BG); y += lh;
    draw_str(6, y, "  ESP-IDF 5.5", C_GRAY, C_BG); y += lh;
    draw_str(6, y, "  ESP32-S3", C_GRAY, C_BG);

    /* Footer */
    fill_rect(0, TFT_HEIGHT - 12, TFT_WIDTH, 12, C_HEADER);
    draw_hline(0, TFT_HEIGHT - 12, TFT_WIDTH, C_DIVIDER);
    draw_str(4, TFT_HEIGHT - 9, "1:Back", C_GRAY, C_HEADER);

    flush_fb();
}

/* ═══════════════════════════════════════════════════════════════════════
 * Special pages
 * ═══════════════════════════════════════════════════════════════════════ */
static void draw_factory_warn_page(void)
{
    memset(fb, 0, sizeof(fb));
    fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, C_RED_BG);

    /* Warning icon (big X) */
    int cx = TFT_WIDTH / 2;
    fill_rect(cx - 16, 30, 32, 4, C_RED);
    fill_rect(cx - 2, 16, 4, 32, C_RED);

    draw_centered(56, "FACTORY RESET", C_RED, C_RED_BG);
    draw_centered(72, "Hold button 10s", C_WHITE, C_RED_BG);
    draw_centered(86, "to confirm", C_WHITE, C_RED_BG);

    /* Progress bar */
    int64_t elapsed = (esp_timer_get_time() / 1000) - warn_state_enter_time;
    int pct = (int)(elapsed * 100 / 10000);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int bar_x = 14, bar_y = 110, bar_w = TFT_WIDTH - 28, bar_h = 16;
    fill_rect(bar_x, bar_y, bar_w, bar_h, C_DARK);
    fill_rect(bar_x + 1, bar_y + 1, (bar_w - 2) * pct / 100, bar_h - 2, C_RED);

    char buf[16];
    int secs = (int)(elapsed / 1000) + 1;
    if (secs > 10) secs = 10;
    if (secs < 1) secs = 1;
    snprintf(buf, sizeof(buf), "%ds / 10s", secs);
    draw_centered(bar_y + bar_h + 8, buf, C_RED, C_RED_BG);

    /* Warning text */
    draw_centered(155, "This will erase", C_WHITE, C_RED_BG);
    draw_centered(170, "ALL settings", C_WHITE, C_RED_BG);
    draw_centered(190, "and reboot", C_WHITE, C_RED_BG);

    flush_fb();
}

static void draw_voice_recording_page(void)
{
    memset(fb, 0, sizeof(fb));
    fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, C_RED_BG);

    /* Blinking record dot */
    int64_t t = esp_timer_get_time() / 150000;
    uint16_t dot_color = (t % 2) ? C_RED : C_WHITE;
    fill_rect(18, 30, 18, 18, dot_color);

    draw_str(44, 34, "RECORDING", C_WHITE, C_RED_BG);

    /* Mic icon */
    int mx = TFT_WIDTH / 2 - 30;
    fill_rect(mx, 75, 60, 70, C_WHITE);
    fill_rect(mx + 6, 81, 48, 58, C_RED_BG);
    fill_rect(mx - 4, 133, 68, 8, C_WHITE);
    fill_rect(mx + 4, 137, 52, 8, C_RED_BG);

    draw_centered(165, "SPEAK NOW", C_WHITE, C_RED_BG);

    /* Animated bars */
    int bar_t = (int)(esp_timer_get_time() / 80000);
    for (int i = 0; i < 5; i++) {
        int bh = 6 + ((bar_t + i * 3) % 8) * 3;
        int bx = mx + 10 + i * 10;
        fill_rect(bx, 150 - bh, 6, bh, C_WHITE);
    }

    flush_fb();
}

static void draw_pairing_page(void)
{
    memset(fb, 0, sizeof(fb));
    fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, C_BG);

    /* Header */
    fill_rect(0, 0, TFT_WIDTH, 14, C_HEADER);
    draw_centered(3, "PAIRING MODE", C_BLUE, C_HEADER);

    /* Bluetooth icon (stylized B) */
    int bx = TFT_WIDTH / 2 - 12;
    fill_rect(bx, 40, 24, 32, C_BLUE);
    fill_rect(bx + 6, 46, 12, 20, C_BG);
    fill_rect(bx + 6, 46, 4, 8, C_BLUE);
    fill_rect(bx + 6, 58, 4, 8, C_BLUE);

    draw_centered(85, "BLE-KVM is", C_WHITE, C_BG);
    draw_centered(100, "discoverable", C_WHITE, C_BG);

    /* Pulsing indicator */
    int64_t pt = esp_timer_get_time() / 300000;
    int dot_count = (pt % 4) + 1;
    char dots[8] = "";
    for (int i = 0; i < dot_count; i++) dots[i] = '.';
    int dx = (TFT_WIDTH - (int)strlen(dots) * 6) / 2;
    draw_str(dx, 120, dots, C_BLUE, C_BG);

    draw_centered(140, "Connect from your", C_DIM_WHITE, C_BG);
    draw_centered(155, "PC or phone", C_DIM_WHITE, C_BG);

    /* Web auth hint */
    draw_centered(185, "Web: double-click", C_GRAY, C_BG);
    draw_centered(200, "button to authorize", C_GRAY, C_BG);

    flush_fb();
}

/* ═══════════════════════════════════════════════════════════════════════
 * Task
 * ═══════════════════════════════════════════════════════════════════════ */
static void tft_task(void *arg)
{
    int loop_count = 0;
    while (1) {
        if (loop_count == 0) {
            ESP_LOGI(TAG, "TFT task running, state=%d page=%d", current_state, current_page);
        }
        loop_count++;
        indicator_state_t local_state;
        int local_page;
        portENTER_CRITICAL(&tft_spinlock);
        local_state = current_state;
        local_page = current_page;
        portEXIT_CRITICAL(&tft_spinlock);

        switch (local_state) {
        case IND_FACTORY_WARN:
            draw_factory_warn_page();
            break;
        case IND_VOICE_RECORDING:
            draw_voice_recording_page();
            break;
        case IND_PAIRING:
            draw_pairing_page();
            break;
        default:
            if (local_page == 0)
                draw_status_page();
            else
                draw_debug_page();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

void tft_display_toggle_page(void)
{
    portENTER_CRITICAL(&tft_spinlock);
    current_page = (current_page + 1) % 2;
    portEXIT_CRITICAL(&tft_spinlock);
}

/* ── Backlight ─────────────────────────────────────────────────────── */
static void init_backlight(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 256,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = TFT_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = 7,
        .timer_sel = LEDC_TIMER_0,
        .duty = 128,
    };
    ledc_channel_config(&c);
}

esp_lcd_panel_handle_t tft_display_get_panel(void)
{
    return panel;
}

void tft_display_freeze(bool freeze)
{
    if (freeze) {
        esp_lcd_panel_disp_on_off(panel, false);  /* display off */
    } else {
        esp_lcd_panel_disp_on_off(panel, true);   /* display on */
    }
}

/* ── PMIC power init ───────────────────────────────────────────────── */
static i2c_master_bus_handle_t i2c_bus;

static void init_pmic_power(void)
{
    i2c_master_bus_config_t ic = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&ic, &i2c_bus));

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dc, &dev));

    /* GPIO2: L3B Enable + LCD Power On (M5GFX init sequence) */
    uint8_t r;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, (uint8_t[]){0x16}, 1, &r, 1, 100));
    r &= ~(1 << 2);
    ESP_ERROR_CHECK(i2c_master_transmit(dev, (uint8_t[]){0x16, r}, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, (uint8_t[]){0x10}, 1, &r, 1, 100));
    r |= (1 << 2);
    ESP_ERROR_CHECK(i2c_master_transmit(dev, (uint8_t[]){0x10, r}, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, (uint8_t[]){0x13}, 1, &r, 1, 100));
    r &= ~(1 << 2);
    ESP_ERROR_CHECK(i2c_master_transmit(dev, (uint8_t[]){0x13, r}, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, (uint8_t[]){0x11}, 1, &r, 1, 100));
    r |= (1 << 2);
    ESP_ERROR_CHECK(i2c_master_transmit(dev, (uint8_t[]){0x11, r}, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit(dev, (uint8_t[]){0x09, 0x00}, 2, 100));
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev));
}

i2c_master_bus_handle_t tft_display_get_i2c_bus(void) { return i2c_bus; }

/* ── Init ──────────────────────────────────────────────────────────── */
void tft_display_init(void)
{
    init_pmic_power();

    spi_bus_config_t bc = {
        .sclk_io_num = TFT_SCLK_GPIO,
        .mosi_io_num = TFT_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TFT_HEIGHT * 80 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bc, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = TFT_DC_GPIO,
        .cs_gpio_num = TFT_CS_GPIO,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &ioc, &io));

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, TFT_OFFSET_X, TFT_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    vTaskDelay(pdMS_TO_TICKS(120));

    init_backlight();
    memset(fb, 0, sizeof(fb));
    flush_fb();

    xTaskCreatePinnedToCore(tft_task, "tft_disp", 8192, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "TFT display initialized (core 1, stack 8KB)");
}

void tft_display_set_state(indicator_state_t state)
{
    portENTER_CRITICAL(&tft_spinlock);
    if (state == IND_FACTORY_WARN && current_state != IND_FACTORY_WARN)
        warn_state_enter_time = esp_timer_get_time() / 1000;
    current_state = state;
    portEXIT_CRITICAL(&tft_spinlock);
}
#endif /* HAS_TFT_DISPLAY */
