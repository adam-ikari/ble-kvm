# Low-Power Sleep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement two-stage low-power sleep (screen-off → sleep) for battery-powered hardware (M5StickS3), with mode-dependent sleep triggers (KVM: all HID disconnected; PPT: IMU idle) and BLE-maintained advertising during sleep with LED breathing.

**Architecture:** Add a `power_manager` subsystem that orchestrates screen-off and sleep state machines. It monitors connection state (via `switch_manager` callbacks), IMU state (via `imu_driver`), and user activity to transition between active → screen-off → sleep → wake. Sleep uses ESP light sleep with BLE advertising kept alive. LED breathing implemented in `rgb_led.c` for the breathing indicator during sleep.

**Tech Stack:** ESP-IDF v5.5, C (ESP32-S3), NimBLE BLE stack

---

### Task 1: Add sleep config fields to kvm_config_t and NVS persistence

**Files:**
- Modify: `main/config_manager.h:41-48` (add fields to struct)
- Modify: `main/config_manager.c:1-258` (add NVS read/write for new fields)

- [ ] **Step 1: Add `screen_off_timeout_sec` and `sleep_timeout_sec` to config struct**

In `main/config_manager.h`, add after `anti_idle_interval_sec`:

```c
    bool anti_idle_enabled;
    uint16_t anti_idle_interval_sec;
    uint16_t screen_off_timeout_sec;   /* seconds, 0 = never, default 30, battery only */
    uint16_t sleep_timeout_sec;        /* seconds, 0 = never, default 60, battery only */
```

- [ ] **Step 2: Add save/load for new fields in config_manager.c**

Find the NVS read block (look for `anti_idle_interval_sec` reads) and add:

```c
    /* In the NVS read section */
    err = nvs_get_u16(handle, "scr_off_to", &cfg->screen_off_timeout_sec);
    if (err != ESP_OK) {
        cfg->screen_off_timeout_sec = 30;
    }
    err = nvs_get_u16(handle, "sleep_to", &cfg->sleep_timeout_sec);
    if (err != ESP_OK) {
        cfg->sleep_timeout_sec = 60;
    }
```

Find the save functions area and add after `config_save_anti_idle`:

```c
void config_save_sleep(void)
{
    nvs_handle_t handle;
    if (nvs_open("config", NVS_READWRITE, &handle) != ESP_OK) return;
    const kvm_config_t *cfg = config_get();
    nvs_set_u16(handle, "scr_off_to", cfg->screen_off_timeout_sec);
    nvs_set_u16(handle, "sleep_to", cfg->sleep_timeout_sec);
    nvs_commit(handle);
    nvs_close(handle);
}
```

Add declaration in `config_manager.h`:

```c
void config_save_sleep(void);
```

- [ ] **Step 3: Build and verify compilation**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

Expected: Build succeeds (no functional change yet).

- [ ] **Step 4: Commit**

```bash
git add main/config_manager.h main/config_manager.c
git commit -m "feat: add screen_off_timeout_sec and sleep_timeout_sec config fields"
```

---

### Task 2: Add IND_SLEEP indicator state and LED breathing

**Files:**
- Modify: `main/indicator.h:3-11` (add `IND_SLEEP` to enum)
- Modify: `main/indicator.c:19-43` (wire through)
- Modify: `main/rgb_led.c:46-84` (add breathing pattern for StampS3)
- Modify: `main/gpio_led.c:1-81` (add breathing for Generic board)

- [ ] **Step 1: Add `IND_SLEEP` to indicator_state_t**

In `main/indicator.h`, add before `} indicator_state_t;`:

```c
    IND_PC1_ACTIVE,
    IND_PC2_ACTIVE,
    IND_PC3_ACTIVE,
    IND_NO_PC,
    IND_PAIRING,
    IND_VOICE_RECORDING,
    IND_FACTORY_WARN,
    IND_SLEEP,            /* <-- ADD: device in low-power sleep, LED breathing */
} indicator_state_t;
```

- [ ] **Step 2: Add breathing pattern to rgb_led.c**

In `main/rgb_led.c`, add a case to the `rgb_task` switch statement before the closing `}` of the switch:

```c
        case IND_SLEEP: {
            /* Breathing: slow sine-like fade in/out using PWM-duty steps.
             * 2-second cycle: 1s fade up, 1s fade down */
            static int breath_step = 0;
            static int breath_dir = 1;
            int brightness = breath_step * 255 / 50;  /* 50 steps */
            /* Show current owner color at breathing brightness */
            const kvm_config_t *cfg = config_get();
            uint8_t r = 0, g = 0, b = 0;
            if (cfg->active_pc == 1)      { g = brightness; }
            else if (cfg->active_pc == 2) { b = brightness; }
            else if (cfg->active_pc == 3) { r = brightness / 2; b = brightness; }
            send_rgb(r, g, b);
            breath_step += breath_dir;
            if (breath_step >= 50) breath_dir = -1;
            if (breath_step <= 0)  breath_dir = 1;
            vTaskDelay(pdMS_TO_TICKS(20));  /* 20ms * 50 = 1000ms per half-cycle */
            break;
        }
```

Add `#include "config_manager.h"` at the top of the file if not already present.

- [ ] **Step 3: Add IND_SLEEP handler to gpio_led.c**

Read `main/gpio_led.c` first, then add:

```c
        case IND_SLEEP:
            /* Breathing via slow blink with PWM-like duty cycle. Since GPIO LEDs
             * don't support PWM on generic board, use slow 2-second blink. */
            led_set_level(LED1_GPIO, (breath_counter < 50) ? 1 : 0);
            led_set_level(LED2_GPIO, (breath_counter < 50) ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(20));
            breath_counter = (breath_counter + 1) % 100;
            break;
```

Note: This requires a `static int breath_counter = 0` variable in the gpio_led task.

- [ ] **Step 4: Build and verify compilation**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add main/indicator.h main/indicator.c main/rgb_led.c main/gpio_led.c
git commit -m "feat: add IND_SLEEP indicator state with LED breathing"
```

---

### Task 3: Create power_manager sleep state machine

**Files:**
- Modify: `main/power_manager.h:1-11` (add sleep state machine API)
- Modify: `main/power_manager.c:1-91` (implement state machine)

- [ ] **Step 1: Read current power_manager.h and power_manager.c**

```bash
cat main/power_manager.h main/power_manager.c
```

(Already read above. `power_manager_init`, `is_charging`, `get_battery_percent`, `get_battery_voltage_mv`, `is_usb_powered` are defined.)

- [ ] **Step 2: Add sleep state machine declarations to power_manager.h**

Replace `main/power_manager.h` with:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

/* ── Battery / PMIC ──────────────────────────────────────────────── */
void power_manager_init(i2c_master_bus_handle_t bus);
bool power_manager_is_charging(void);
uint8_t power_manager_get_battery_percent(void);
uint16_t power_manager_get_battery_voltage_mv(void);
bool power_manager_is_usb_powered(void);

/* ── Sleep state machine (battery hardware only) ─────────────────── */
typedef enum {
    PM_STATE_ACTIVE,       /* fully awake */
    PM_STATE_SCREEN_OFF,   /* backlight off, LCD frozen, everything else running */
    PM_STATE_SLEEP,        /* light sleep: BLE advertising, LED breathing, rest off */
} pm_sleep_state_t;

#if HAS_BATTERY
void pm_sleep_init(void);
pm_sleep_state_t pm_sleep_get_state(void);
void pm_sleep_on_activity(void);        /* call on any user interaction */
void pm_sleep_on_pc_connected(uint8_t pc_id);
void pm_sleep_on_pc_disconnected(uint8_t pc_id);
void pm_sleep_on_imu_motion(void);      /* call on IMU change detected */
void pm_sleep_enter_force(void);        /* force sleep (e.g. long press) — not used in v1.0 */
#else
static inline void pm_sleep_init(void) {}
static inline pm_sleep_state_t pm_sleep_get_state(void) { return PM_STATE_ACTIVE; }
static inline void pm_sleep_on_activity(void) {}
static inline void pm_sleep_on_pc_connected(uint8_t pc_id) {}
static inline void pm_sleep_on_pc_disconnected(uint8_t pc_id) {}
static inline void pm_sleep_on_imu_motion(void) {}
static inline void pm_sleep_enter_force(void) {}
#endif
```

- [ ] **Step 3: Implement sleep state machine in power_manager.c**

Add after the existing battery functions in `main/power_manager.c`:

```c
#if HAS_BATTERY

#include "config_manager.h"
#include "indicator.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
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
    /* Freeze LCD and kill backlight */
    tft_display_freeze(true);
    /* Backlight off: duty = 0 on LEDC channel 7 */
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
    esp_sleep_enable_timer_wakeup(5 * 1000000ULL);  /* 5s periodic wake to check conditions */
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

    /* New connection — cancel sleep */
    if (sleep_state == PM_STATE_SLEEP) {
        /* Woken by BLE connection; esp_light_sleep_start() returns automatically */
    }
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

#endif /* HAS_BATTERY */
```

- [ ] **Step 4: Expose TFT panel and freeze function from tft_display.c**

Check if these already exist:

```bash
grep -n "tft_display_get_panel\|tft_display_freeze\|extern.*tft_display" main/tft_display.c main/tft_display.h 2>/dev/null
```

If `tft_display.h` doesn't exist, add it:

```c
#pragma once
#include "esp_lcd_panel.h"
#if HAS_TFT_DISPLAY
esp_lcd_panel_handle_t tft_display_get_panel(void);
void tft_display_freeze(bool freeze);
#endif
```

In `tft_display.c`, add:

```c
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
```

- [ ] **Step 5: Build and verify compilation**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add main/power_manager.h main/power_manager.c main/tft_display.h main/tft_display.c
git commit -m "feat: implement sleep state machine with screen-off and light-sleep stages"
```

---

### Task 4: Wire sleep callbacks into main.c, switch_manager, and IMU

**Files:**
- Modify: `main/main.c:60-144` (init pm_sleep)
- Modify: `main/switch_manager.c:408-439` (call pm_sleep_on_pc_connected/disconnected)
- Modify: `main/switch_manager.c:100-138` (call pm_sleep_on_activity on button)

- [ ] **Step 1: Init sleep state machine in main.c**

In `main/app_main()`, add after `power_manager_init(i2c_bus);`:

```c
#if HAS_BATTERY
    pm_sleep_init();
#endif
```

Add `#include "power_manager.h"` at top (already present via `#if HAS_BATTERY` block).

- [ ] **Step 2: Wire PC connect/disconnect callbacks in switch_manager.c**

In `switch_manager_on_pc_connected()`, add at end:

```c
#if HAS_BATTERY
    pm_sleep_on_pc_connected(pc_id);
#endif
```

In `switch_manager_on_pc_disconnected()`, add at end:

```c
#if HAS_BATTERY
    pm_sleep_on_pc_disconnected(pc_id);
#endif
```

Add `#include "power_manager.h"` to `switch_manager.c` includes.

- [ ] **Step 3: Wire activity callback on button press**

In `switch_task_func()`, at the top of each command handler that represents user interaction (CMD_SWITCH, CMD_SECONDARY, CMD_MODE_CYCLE), add:

```c
#if HAS_BATTERY
            pm_sleep_on_activity();
#endif
```

The simplest approach: add it once at the start of the `xQueueReceive` success block:

```c
        if (xQueueReceive(switch_queue, &cmd, pdMS_TO_TICKS(100))) {
#if HAS_BATTERY
            pm_sleep_on_activity();
#endif
            if (cmd == CMD_SWITCH) {
```

- [ ] **Step 4: Wire IMU motion callback**

Read `main/imu_driver.c` to find where IMU data is processed. Add at the point where new IMU readings are received:

```c
#if HAS_BATTERY
    extern void pm_sleep_on_imu_motion(void);
    pm_sleep_on_imu_motion();
#endif
```

- [ ] **Step 5: Wire Web API activity**

In `main/web_server.c`, find the websocket handler or API handlers. Add `pm_sleep_on_activity()` in the POST `/api/owner` handler and POST `/api/config` handler:

```c
#if HAS_BATTERY
    pm_sleep_on_activity();
#endif
```

Add `#include "power_manager.h"` to web_server.c.

- [ ] **Step 6: Build and verify compilation**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add main/main.c main/switch_manager.c main/imu_driver.c main/web_server.c
git commit -m "feat: wire sleep callbacks into main, switch_manager, IMU, and web_server"
```

---

### Task 5: Add sleep and screen-off config to Web API

**Files:**
- Modify: `main/web_server.c` (add config keys and status fields)

- [ ] **Step 1: Add sleep config keys to POST /api/config handler**

Find the config key parsing in `web_server.c` (look for `anti_idle_interval_sec` handling). Add:

```c
    } else if (strcmp(key, "screen_off_timeout_sec") == 0) {
        cfg->screen_off_timeout_sec = (uint16_t)val_int;
    } else if (strcmp(key, "sleep_timeout_sec") == 0) {
        cfg->sleep_timeout_sec = (uint16_t)val_int;
```

After parsing loop, if any sleep-related key was set, call `config_save_sleep()`.

- [ ] **Step 2: Add sleep fields to GET /api/status response**

Find the JSON construction for `/api/status`. Add:

```c
    cJSON_AddNumberToObject(root, "screen_off_timeout_sec", cfg->screen_off_timeout_sec);
    cJSON_AddNumberToObject(root, "sleep_timeout_sec", cfg->sleep_timeout_sec);
#if HAS_BATTERY
    cJSON_AddStringToObject(root, "sleep_state",
        pm_sleep_get_state() == PM_STATE_ACTIVE ? "active" :
        pm_sleep_get_state() == PM_STATE_SCREEN_OFF ? "screen_off" : "sleep");
#endif
```

- [ ] **Step 3: Add to GET /api/config response**

In the config response, add:

```c
    cJSON_AddNumberToObject(root, "screen_off_timeout_sec", cfg->screen_off_timeout_sec);
    cJSON_AddNumberToObject(root, "sleep_timeout_sec", cfg->sleep_timeout_sec);
```

- [ ] **Step 4: Build, verify, commit**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

```bash
git add main/web_server.c
git commit -m "feat: add sleep and screen-off config to Web API"
```

---

### Task 6: Single-button long press factory reset — already implemented

> **STATUS: ALREADY DONE — verify only, do not re-implement.**
>
> The single-button factory reset already uses the two-step confirmation
> decided for v1.1: **5s warning (LED fast blink) → continue holding to 10s
> = execute factory reset; release between 5–10s = cancel.** This matches
> `docs/superpowers/plans/2026-06-13-spec-v1.1-changes.md` Task 4 and the
> resolved decision on the factory-reset timing conflict (5s warn + 10s exec).
>
> `main/switch_manager.c` already has: `FACTORY_WARN_MS 5000`,
> `FACTORY_RST_MS 10000`, `CMD_FACTORY_WARN`/`CMD_FACTORY_CANCEL` commands,
> `factory_warned` flag, and the warning/cancel logic in `voice_start_timer_cb`
> (lines ~246–260) and `button_isr_handler` (lines ~298–305). The earlier
> draft of this task proposed `FACTORY_RST_MS 5000` with no warning — that
> was **superseded** and must NOT be applied; it would destroy the two-step
> confirmation.

**Files:**
- Verify: `main/switch_manager.c:32-35` (timing defines), `:246-260` (warn/rst/cancel in timer cb), `:298-306` (cancel on release)

- [ ] **Step 1: Verify defines are correct**

`main/switch_manager.c` should contain:
```c
#define FACTORY_WARN_MS    5000
#define FACTORY_RST_MS     10000
```
If `FACTORY_RST_MS` is 5000, it was regressed — restore to 10000.

- [ ] **Step 2: Verify warn/rst/cancel flow is present**

In `voice_start_timer_cb` (single-button branch, `#else`):
- `duration >= FACTORY_RST_MS && !long_press_triggered` → queue `CMD_FACTORY_RST`
- `duration >= FACTORY_WARN_MS && !factory_warned` → queue `CMD_FACTORY_WARN`

In `button_isr_handler` release block (single-button, `!long_press_triggered`):
- `if (factory_warned)` → reset flag, queue `CMD_FACTORY_CANCEL`

And in the switch task, `CMD_FACTORY_WARN` → `indicator_set_state(IND_PAIRING)`,
`CMD_FACTORY_CANCEL` → `update_led_state()`.

- [ ] **Step 3: Build, verify, commit (only if a fix was needed)**

```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -20
```

If no changes were needed (expected case), skip the commit — the code is
already correct. Only commit if a regression was found and fixed.

---

### Task 7: Integrate and smoke test

**Files:**
- Test: `test/` directory (manual test checklist)

- [ ] **Step 1: Full build for all board variants**

```bash
cd /home/gem/project/ble-kvm
BOARD=m5sticks3 idf.py build 2>&1 | tail -5
BOARD=m5stamps3 idf.py build 2>&1 | tail -5
BOARD=default idf.py build 2>&1 | tail -5
```

Expected: All three board variants build successfully.

- [ ] **Step 2: Verify M5StampS3 (non-battery) doesn't include sleep code**

```bash
grep -r "pm_sleep\|PM_STATE\|IND_SLEEP" build/ 2>/dev/null | head -10
```

The symbols may appear in the binary but the code paths should be guarded by `#if HAS_BATTERY`. Verify that `pm_sleep_init` is never called on StampS3.

- [ ] **Step 3: Manual test plan document**

Create `test/sleep-test-checklist.md` with:

```markdown
# Low-Power Sleep Test Checklist

## Hardware: M5StickS3 (battery)

### Screen Off Test
1. Power on StickS3 with battery
2. Wait for screen_off_timeout_sec (default 30s) with no interaction
3. [ ] Screen backlight turns off after timeout
4. [ ] Press button — screen lights up immediately
5. [ ] Screen-off timer resets on button press

### KVM Sleep Test
1. Disconnect all BLE and USB connections
2. Wait for sleep_timeout_sec (default 60s)
3. [ ] Device enters sleep (LED breathing)
4. [ ] Connect BLE from PC — device wakes up
5. [ ] LED returns to solid, screen lights up

### PPT Sleep Test
1. Switch to PPT mode (side button)
2. Leave device still (no IMU motion)
3. Wait for sleep_timeout_sec
4. [ ] Device enters sleep
5. [ ] Move device — IMU motion detected, sleep timer resets

### Wake Test
1. In sleep state
2. [ ] BLE connection from PC wakes device
3. [ ] Physical button press wakes device
4. [ ] USB VBUS (plug into PC) wakes device

### Non-Battery Hardware (M5StampS3)
1. [ ] Sleep config fields not visible in web UI
2. [ ] No sleep behavior, no crashes
```

- [ ] **Step 4: Final commit**

```bash
git add test/sleep-test-checklist.md
git commit -m "test: add low-power sleep manual test checklist"
```

---
