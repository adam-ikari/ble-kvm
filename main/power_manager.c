#include "power_manager.h"
#include "board.h"
#if HAS_BATTERY

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "power";
static i2c_master_dev_handle_t pmic_dev;
static SemaphoreHandle_t pmic_mutex = NULL;
static int64_t last_activity_time = 0;

#define SLEEP_IDLE_MS      (5 * 60 * 1000)
#define SLEEP_DEEP_MS      (15 * 60 * 1000)

static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    if (!pmic_mutex) return ESP_FAIL;
    xSemaphoreTake(pmic_mutex, pdMS_TO_TICKS(200));
    esp_err_t ret = i2c_master_transmit_receive(pmic_dev, &reg, 1, val, 1, 100);
    xSemaphoreGive(pmic_mutex);
    return ret;
}

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    if (!pmic_mutex) return ESP_FAIL;
    xSemaphoreTake(pmic_mutex, pdMS_TO_TICKS(200));
    uint8_t buf[] = {reg, val};
    esp_err_t ret = i2c_master_transmit(pmic_dev, buf, sizeof(buf), 100);
    xSemaphoreGive(pmic_mutex);
    return ret;
}

void power_manager_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &pmic_dev));
    pmic_mutex = xSemaphoreCreateMutex();

    last_activity_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Power manager initialized");
}

bool power_manager_is_charging(void)
{
    uint8_t val = 0;
    if (pmic_read(0x01, &val) != ESP_OK) return false;
    return (val & 0x01) != 0;
}

uint8_t power_manager_get_battery_percent(void)
{
    uint16_t mv = power_manager_get_battery_voltage_mv();
    if (mv == 0) return 0;
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    return (uint8_t)((mv - 3000) * 100 / 1200);
}

uint16_t power_manager_get_battery_voltage_mv(void)
{
    uint8_t hi = 0, lo = 0;
    if (pmic_read(0x78, &hi) != ESP_OK) return 0;
    if (pmic_read(0x79, &lo) != ESP_OK) return 0;
    /* AXP2101: 12-bit ADC, 1.1mV per LSB, value in hi[7:0] << 4 | lo[7:4] */
    uint16_t raw = ((uint16_t)(hi & 0xFF) << 4) | ((lo >> 4) & 0x0F);
    return (uint16_t)(raw * 11 / 10);  /* raw * 1.1mV */
}

bool power_manager_is_usb_powered(void)
{
    uint8_t val = 0;
    if (pmic_read(0x00, &val) != ESP_OK) return true;
    return (val & 0x80) != 0;
}

/* ── Sleep state machine ─────────────────────────────────────────── */

#include "config_manager.h"
#include "indicator.h"
#include "esp_wifi.h"
#if HAS_TFT_DISPLAY
#include "esp_lcd_panel_ops.h"
extern esp_lcd_panel_handle_t tft_display_get_panel(void);
extern void tft_display_freeze(bool freeze);
#endif

static const char *TAG_SLEEP = "pm_sleep";
static pm_sleep_state_t sleep_state = PM_STATE_ACTIVE;
static esp_timer_handle_t screen_off_timer;
static esp_timer_handle_t sleep_timer;
static int connected_pc_count = 0;
static portMUX_TYPE pm_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── helpers ─────────────────────────────────────────────────────── */

static bool all_pcs_disconnected(void)
{
    return connected_pc_count == 0;
}

static void enter_screen_off(void)
{
    if (sleep_state != PM_STATE_ACTIVE) return;
    sleep_state = PM_STATE_SCREEN_OFF;
    ESP_LOGI(TAG_SLEEP, "Screen off");

#if HAS_TFT_DISPLAY
    tft_display_freeze(true);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
#endif
}

static void exit_screen_off(void)
{
    if (sleep_state != PM_STATE_SCREEN_OFF) return;
    sleep_state = PM_STATE_ACTIVE;
    ESP_LOGI(TAG_SLEEP, "Screen on");

#if HAS_TFT_DISPLAY
    tft_display_freeze(false);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
#endif
}

static void enter_sleep(void)
{
    if (sleep_state == PM_STATE_SLEEP) return;
    sleep_state = PM_STATE_SLEEP;
    ESP_LOGI(TAG_SLEEP, "Entering sleep");

    indicator_set_state(IND_SLEEP);

#if HAS_TFT_DISPLAY
    tft_display_freeze(true);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
#endif

    /* Wi-Fi off */
    esp_wifi_stop();

    /* Configure wake sources then enter light sleep.
     * BLE advertising stays active via NimBLE's own sleep integration.
     * GPIO wake on button press, timer wake for periodic checks. */
    esp_sleep_enable_timer_wakeup(5 * 1000000ULL);  /* 5s periodic wake */
    esp_light_sleep_start();

    /* ── Woke up ── */
    ESP_LOGI(TAG_SLEEP, "Woke from sleep");
    sleep_state = PM_STATE_ACTIVE;

    /* Re-enable Wi-Fi */
    esp_wifi_start();

    indicator_set_state(IND_NO_PC);  /* will be corrected by update_led_state */
}

/* ── timer callbacks ─────────────────────────────────────────────── */

static void screen_off_timer_cb(void *arg)
{
    const kvm_config_t *cfg = config_get();
    if (cfg->screen_off_timeout_sec == 0) return;
    enter_screen_off();
}

static void sleep_timer_cb(void *arg)
{
    const kvm_config_t *cfg = config_get();
    if (cfg->sleep_timeout_sec == 0) return;

    uint8_t mode = config_get()->input_mode;
    if (mode == 0 /* KVM */) {
        if (!all_pcs_disconnected()) return;
    }
    /* PPT mode: IMU-idle check handled by pm_sleep_on_imu_motion()
     * resetting the timer. If we reach here in PPT mode, IMU has been
     * idle long enough. */

    enter_sleep();
}

/* ── timer management ────────────────────────────────────────────── */

static void restart_screen_off_timer(void)
{
    const kvm_config_t *cfg = config_get();
    esp_timer_stop(screen_off_timer);
    if (cfg->screen_off_timeout_sec > 0 && sleep_state == PM_STATE_ACTIVE) {
        esp_timer_start_once(screen_off_timer,
                             (uint64_t)cfg->screen_off_timeout_sec * 1000000);
    }
}

static void restart_sleep_timer(void)
{
    const kvm_config_t *cfg = config_get();
    esp_timer_stop(sleep_timer);
    if (cfg->sleep_timeout_sec > 0 && sleep_state != PM_STATE_SLEEP) {
        esp_timer_start_once(sleep_timer,
                             (uint64_t)cfg->sleep_timeout_sec * 1000000);
    }
}

/* ── public API ──────────────────────────────────────────────────── */

void pm_sleep_init(void)
{
    const esp_timer_create_args_t scr_args = {
        .callback = screen_off_timer_cb,
        .name = "screen_off",
    };
    esp_timer_create(&scr_args, &screen_off_timer);

    const esp_timer_create_args_t slp_args = {
        .callback = sleep_timer_cb,
        .name = "sleep_tmr",
    };
    esp_timer_create(&slp_args, &sleep_timer);

    /* Start timers */
    const kvm_config_t *cfg = config_get();
    if (cfg->screen_off_timeout_sec > 0) {
        restart_screen_off_timer();
    }
    if (cfg->sleep_timeout_sec > 0) {
        restart_sleep_timer();
    }

    ESP_LOGI(TAG_SLEEP, "Sleep state machine init (scr=%ds, slp=%ds)",
             cfg->screen_off_timeout_sec, cfg->sleep_timeout_sec);
}

pm_sleep_state_t pm_sleep_get_state(void)
{
    return sleep_state;
}

void pm_sleep_on_activity(void)
{
    if (sleep_state == PM_STATE_SCREEN_OFF) {
        exit_screen_off();
    }
    restart_screen_off_timer();
}

void pm_sleep_on_pc_connected(uint8_t pc_id)
{
    portENTER_CRITICAL(&pm_spinlock);
    connected_pc_count++;
    portEXIT_CRITICAL(&pm_spinlock);

    /* New connection — cancel sleep timer */
    esp_timer_stop(sleep_timer);
    pm_sleep_on_activity();
}

void pm_sleep_on_pc_disconnected(uint8_t pc_id)
{
    portENTER_CRITICAL(&pm_spinlock);
    if (connected_pc_count > 0) connected_pc_count--;
    portEXIT_CRITICAL(&pm_spinlock);

    if (all_pcs_disconnected()) {
        restart_sleep_timer();
    }
}

void pm_sleep_on_imu_motion(void)
{
    /* Only relevant in PPT mode; reset sleep timer on IMU activity */
    if (config_get()->input_mode == 1 /* PPT */) {
        restart_sleep_timer();
    }
}

void pm_sleep_enter_force(void)
{
    esp_timer_stop(sleep_timer);
    enter_sleep();
}

#endif // HAS_BATTERY
