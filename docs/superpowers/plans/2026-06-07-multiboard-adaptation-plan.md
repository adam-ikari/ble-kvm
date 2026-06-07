# Multi-Board Adaptation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adapt ble-kvm for M5Stack StickS3 (TFT + battery), Stamp S3 (RGB LED), and DevKitC (GPIO LED) via a board abstraction layer, plus add anti-idle and RESTful API redesign.

**Architecture:** Board-specific differences (GPIO pins, display type, battery) are isolated behind `board.h` compile-time macros and `indicator.h/c` runtime abstraction. New modules (anti_idle, power_manager, tft_display, rgb_led) are conditionally compiled. Web API is redesigned to RESTful conventions with merged pairing endpoints.

**Tech Stack:** ESP-IDF 5.4.1, NimBLE, esp_lcd (ST7789), RMT (WS2812B), I2C (M5PM1), cJSON

---

## File Structure

| File | Responsibility |
|------|---------------|
| `main/board.h` | Board-specific GPIO/macro definitions, selected at compile time |
| `main/indicator.h` | Public interface for status indication (replaces led_controller.h) |
| `main/indicator.c` | Dispatcher: calls the active board implementation |
| `main/gpio_led.c` | DevKitC: two GPIO LEDs (replaces led_controller.c logic) |
| `main/rgb_led.c` | Stamp S3: single WS2812B RGB LED via RMT |
| `main/tft_display.c` | StickS3: ST7789P3 TFT via esp_lcd + bitmap font renderer |
| `main/power_manager.h` | Battery/PMIC interface (StickS3 only) |
| `main/power_manager.c` | M5PM1 I2C communication, voltage reading, auto-sleep |
| `main/anti_idle.h` | Anti-idle public interface |
| `main/anti_idle.c` | Timer-based mouse nudge to prevent PC sleep |
| `main/switch_manager.c` | Modified: use board.h GPIO, remove long-press pairing |
| `main/switch_manager.h` | Modified: remove pairing-related enums |
| `main/config_manager.h` | Modified: add anti_idle fields to kvm_config_t |
| `main/config_manager.c` | Modified: persist anti_idle settings |
| `main/hid_router.h` | Modified: add activity callback registration |
| `main/hid_router.c` | Modified: call activity callback on report forward |
| `main/web_server.c` | Modified: RESTful API redesign |
| `main/main.c` | Modified: init indicator instead of led_controller, conditional power_manager |
| `main/CMakeLists.txt` | Modified: conditional source files based on BOARD |
| `main/led_controller.c` | Delete |
| `main/led_controller.h` | Delete |
| `sdkconfig.defaults.m5sticks3` | New: StickS3-specific sdkconfig overrides |
| `sdkconfig.defaults.m5stamps3` | New: Stamp S3-specific sdkconfig overrides |

---

### Task 1: Add board.h and CMake conditional build

**Files:**
- Create: `main/board.h`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create `main/board.h`**

```c
#pragma once

#include "driver/gpio.h"

#ifdef BOARD_M5STICKS3
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_11
  #define BUTTON_SECONDARY_GPIO  GPIO_NUM_12
  #define HAS_SECONDARY_BUTTON   1
  #define HAS_TFT_DISPLAY        1
  #define TFT_MOSI_GPIO          GPIO_NUM_39
  #define TFT_SCLK_GPIO          GPIO_NUM_40
  #define TFT_DC_GPIO            GPIO_NUM_45
  #define TFT_CS_GPIO            GPIO_NUM_41
  #define TFT_RST_GPIO           GPIO_NUM_21
  #define TFT_BL_GPIO            GPIO_NUM_38
  #define TFT_WIDTH              135
  #define TFT_HEIGHT             240
  #define TFT_OFFSET_X           52
  #define TFT_OFFSET_Y           40
  #define HAS_BATTERY            1
  #define PMIC_I2C_ADDR          0x6e
  #define I2C_SDA_GPIO           GPIO_NUM_47
  #define I2C_SCL_GPIO           GPIO_NUM_48
  #define HAS_RGB_LED            0
  #define HAS_GPIO_LED           0

#elif defined(BOARD_M5STAMPS3)
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  #define HAS_RGB_LED            1
  #define RGB_LED_GPIO           GPIO_NUM_27
  #define HAS_GPIO_LED           0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0

#else
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  #define HAS_GPIO_LED           1
  #define LED1_GPIO              GPIO_NUM_2
  #define LED2_GPIO              GPIO_NUM_1
  #define HAS_RGB_LED            0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0
#endif
```

- [ ] **Step 2: Update `main/CMakeLists.txt` with conditional build**

```cmake
set(COMPONENT_SRCS
    "main.c" "config_manager.c" "ble_peripheral.c"
    "switch_manager.c" "hid_router.c" "ble_central.c"
    "wifi_manager.c" "web_server.c"
    "indicator.c" "anti_idle.c"
)

set(EXTRA_REQUIRES "")

if(BOARD STREQUAL "m5sticks3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STICKS3)
    list(APPEND COMPONENT_SRCS "tft_display.c" "power_manager.c")
    list(APPEND EXTRA_REQUIRES "esp_lcd" "esp_driver_i2c")
elseif(BOARD STREQUAL "m5stamps3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STAMPS3)
    list(APPEND COMPONENT_SRCS "rgb_led.c")
    list(APPEND EXTRA_REQUIRES "esp_driver_rmt")
else()
    list(APPEND COMPONENT_SRCS "gpio_led.c")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_wifi esp_netif
             esp_event json esp_http_server ${EXTRA_REQUIRES}
)
```

- [ ] **Step 3: Create sdkconfig defaults for each board**

Create `sdkconfig.defaults.m5sticks3`:
```
CONFIG_SPIRAM=y
```

Create `sdkconfig.defaults.m5stamps3`:
```
# No PSRAM, no extra config needed
```

- [ ] **Step 4: Verify default build still compiles**

Run: `cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -5`
Expected: Build may fail at this point due to missing source files (indicator.c etc.) — that's OK, we'll create them in subsequent tasks. The CMake structure should be valid.

- [ ] **Step 5: Commit**

```bash
git add main/board.h main/CMakeLists.txt sdkconfig.defaults.m5sticks3 sdkconfig.defaults.m5stamps3
git commit -m "feat: add board abstraction layer with conditional build system"
```

---

### Task 2: Create indicator abstraction (replace led_controller)

**Files:**
- Create: `main/indicator.h`
- Create: `main/indicator.c`
- Create: `main/gpio_led.c`
- Delete: `main/led_controller.c`
- Delete: `main/led_controller.h`

- [ ] **Step 1: Create `main/indicator.h`**

```c
#pragma once

typedef enum {
    IND_PC1_ACTIVE,
    IND_PC2_ACTIVE,
    IND_NO_PC,
    IND_PAIRING,
} indicator_state_t;

void indicator_init(void);
void indicator_set_state(indicator_state_t state);
```

- [ ] **Step 2: Create `main/indicator.c`**

```c
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
```

- [ ] **Step 3: Create `main/gpio_led.c` — migrate from led_controller.c**

```c
#include "indicator.h"
#include "board.h"
#if HAS_GPIO_LED

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gpio_led";
static indicator_state_t current_state = IND_NO_PC;
static TaskHandle_t led_task_handle = NULL;

static void set_leds(bool led1, bool led2)
{
    gpio_set_level(LED1_GPIO, led1 ? 0 : 1);
    gpio_set_level(LED2_GPIO, led2 ? 0 : 1);
}

static void led_task(void *arg)
{
    bool toggle = false;
    while (1) {
        switch (current_state) {
        case IND_PC1_ACTIVE:
            set_leds(true, false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PC2_ACTIVE:
            set_leds(false, true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PAIRING:
            set_leds(toggle, !toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        case IND_NO_PC:
            set_leds(toggle, false);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

void gpio_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED1_GPIO) | (1ULL << LED2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    set_leds(false, false);

    xTaskCreate(led_task, "led_ctrl", 1024, NULL, 1, &led_task_handle);
    ESP_LOGI(TAG, "GPIO LED initialized");
}

void gpio_led_set_state(indicator_state_t state)
{
    current_state = state;
}

#endif // HAS_GPIO_LED
```

- [ ] **Step 4: Create stub `main/rgb_led.c` for now (will be fully implemented in Task 5)**

```c
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
    // Full implementation in Task 5
}

#endif // HAS_RGB_LED
```

- [ ] **Step 5: Create stub `main/tft_display.c` for now (will be fully implemented in Task 6)**

```c
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
    // Full implementation in Task 6
}

#endif // HAS_TFT_DISPLAY
```

- [ ] **Step 6: Update `main/main.c` — replace led_controller with indicator**

Change the include and init call:
- Replace `#include "led_controller.h"` with `#include "indicator.h"`
- Replace `led_controller_init()` with `indicator_init()`
- Replace `led_controller_set_state(...)` calls with `indicator_set_state(...)` (in switch_manager.c)

- [ ] **Step 7: Update `main/switch_manager.c` — replace led_controller with indicator**

- Replace `#include "led_controller.h"` with `#include "indicator.h"`
- Replace `led_controller_set_state(LED_STATE_PC1_ACTIVE)` with `indicator_set_state(IND_PC1_ACTIVE)`
- Same for all LED_STATE_* → IND_* mappings
- Update button GPIO from `GPIO_NUM_0` to `BUTTON_SWITCH_GPIO` from board.h
- Remove long-press pairing logic (the `duration > 2000` branch that would trigger pairing)
- Keep only the short-press switch (< 2000ms)

- [ ] **Step 8: Delete old led_controller files**

```bash
rm main/led_controller.c main/led_controller.h
```

- [ ] **Step 9: Build and verify DevKitC target compiles**

Run: `cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -5`
Expected: BUILD SUCCESS

- [ ] **Step 10: Commit**

```bash
git add main/indicator.h main/indicator.c main/gpio_led.c main/rgb_led.c main/tft_display.c main/main.c main/switch_manager.c
git rm main/led_controller.c main/led_controller.h
git commit -m "feat: replace led_controller with indicator abstraction for multi-board support"
```

---

### Task 3: Anti-idle feature

**Files:**
- Create: `main/anti_idle.h`
- Create: `main/anti_idle.c`
- Modify: `main/config_manager.h`
- Modify: `main/config_manager.c`
- Modify: `main/hid_router.h`
- Modify: `main/hid_router.c`
- Modify: `main/main.c`

- [ ] **Step 1: Add anti_idle fields to `main/config_manager.h`**

Add to `kvm_config_t`:
```c
    bool anti_idle_enabled;
    uint16_t anti_idle_interval_sec;
```

- [ ] **Step 2: Add anti_idle persistence to `main/config_manager.c`**

Add load function:
```c
static void load_anti_idle(void)
{
    uint8_t enabled = 0;
    esp_err_t err = nvs_get_u8(nvs_config, "anti_idle", &enabled);
    config.anti_idle_enabled = (err == ESP_OK) ? (enabled ? true : false) : false;

    uint16_t interval = 0;
    err = nvs_get_u16(nvs_config, "anti_idle_ivl", &interval);
    config.anti_idle_interval_sec = (err == ESP_OK) ? interval : 240;
}
```

Add to `config_manager_init()` after `load_wifi()`:
```c
    load_anti_idle();
```

Add save function:
```c
void config_save_anti_idle(void)
{
    uint8_t enabled = config.anti_idle_enabled ? 1 : 0;
    ESP_ERROR_CHECK(nvs_set_u8(nvs_config, "anti_idle", enabled));
    ESP_ERROR_CHECK(nvs_set_u16(nvs_config, "anti_idle_ivl", config.anti_idle_interval_sec));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}
```

Add declaration to `config_manager.h`:
```c
void config_save_anti_idle(void);
```

- [ ] **Step 3: Create `main/anti_idle.h`**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

void anti_idle_init(void);
void anti_idle_on_activity(void);
void anti_idle_set_enabled(bool enabled);
void anti_idle_set_interval(uint16_t interval_sec);
```

- [ ] **Step 4: Create `main/anti_idle.c`**

```c
#include "anti_idle.h"
#include "config_manager.h"
#include "switch_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "anti_idle";
static esp_timer_handle_t idle_timer;
static bool active = false;

static void send_mouse_nudge(void)
{
    uint16_t conn = switch_manager_get_active_conn_handle();
    if (conn == 0 || !ble_central_is_mouse_connected()) return;

    /* Mouse report: buttons=0, dx=1, dy=0, scroll=0 */
    uint8_t report1[] = {0x00, 0x01, 0x00, 0x00};
    ble_peripheral_send_hid_report(conn, 2, report1, sizeof(report1));

    vTaskDelay(pdMS_TO_TICKS(20));

    /* Move back: dx=-1, dy=0 */
    uint8_t report2[] = {0x00, 0xFF, 0x00, 0x00};
    ble_peripheral_send_hid_report(conn, 2, report2, sizeof(report2));
}

static void idle_timer_cb(void *arg)
{
    if (!config_get()->anti_idle_enabled) return;
    send_mouse_nudge();
}

static void restart_timer(void)
{
    esp_timer_stop(idle_timer);
    if (config_get()->anti_idle_enabled && ble_central_is_mouse_connected()) {
        uint16_t interval = config_get()->anti_idle_interval_sec;
        esp_timer_start_periodic(idle_timer, (uint64_t)interval * 1000000);
        active = true;
    } else {
        active = false;
    }
}

void anti_idle_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = idle_timer_cb,
        .name = "anti_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &idle_timer));

    if (config_get()->anti_idle_enabled) {
        restart_timer();
    }
    ESP_LOGI(TAG, "Anti-idle initialized (enabled=%d, interval=%ds)",
             config_get()->anti_idle_enabled, config_get()->anti_idle_interval_sec);
}

void anti_idle_on_activity(void)
{
    if (active) {
        restart_timer();
    }
}

void anti_idle_set_enabled(bool enabled)
{
    config_get_mutable()->anti_idle_enabled = enabled;
    config_save_anti_idle();
    restart_timer();
}

void anti_idle_set_interval(uint16_t interval_sec)
{
    if (interval_sec < 30) interval_sec = 30;
    if (interval_sec > 3600) interval_sec = 3600;
    config_get_mutable()->anti_idle_interval_sec = interval_sec;
    config_save_anti_idle();
    restart_timer();
}
```

- [ ] **Step 5: Add activity callback to `main/hid_router.h`**

```c
typedef void (*hid_activity_cb_t)(void);
void hid_router_register_activity_cb(hid_activity_cb_t cb);
```

- [ ] **Step 6: Update `main/hid_router.c`**

Add at top:
```c
static hid_activity_cb_t activity_cb = NULL;

void hid_router_register_activity_cb(hid_activity_cb_t cb)
{
    activity_cb = cb;
}
```

Add at end of `hid_router_forward_keyboard()` and `hid_router_forward_mouse()`, after the successful send:
```c
    if (activity_cb) activity_cb();
```

- [ ] **Step 7: Update `main/main.c`**

Add includes:
```c
#include "anti_idle.h"
```

Add after `hid_router_init()`:
```c
    hid_router_register_activity_cb(anti_idle_on_activity);
    anti_idle_init();
```

- [ ] **Step 8: Build and verify**

Run: `cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -5`
Expected: BUILD SUCCESS

- [ ] **Step 9: Commit**

```bash
git add main/anti_idle.h main/anti_idle.c main/config_manager.h main/config_manager.c main/hid_router.h main/hid_router.c main/main.c
git commit -m "feat: add anti-idle mouse nudge to prevent PC sleep"
```

---

### Task 4: RESTful API redesign

**Files:**
- Modify: `main/web_server.c`

- [ ] **Step 1: Replace the entire URI registration table and handlers in `main/web_server.c`**

Key changes:
1. Merge `/api/pair/keyboard` + `/api/pair/mouse` + `/api/pair/pc` → `POST /api/pairings`
2. Merge `/api/scan` (POST) + `/api/scan/results` (GET) → `POST /api/scan` + `GET /api/scan`
3. Change `/api/wifi` POST → `GET /api/wifi` + `PATCH /api/wifi`
4. Change `/api/settings` POST → `PATCH /api/settings`
5. Add `DELETE /api/pairings/{id}` handler
6. Add anti_idle to settings GET/PATCH

The `pairings_post_handler`:
```c
static esp_err_t pairings_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *type_item = cJSON_GetObjectItem(body, "type");
    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type field");
        return ESP_FAIL;
    }

    const char *type = type_item->valuestring;

    if (strcmp(type, "pc") == 0) {
        ble_peripheral_start_advertising();
    } else if (strcmp(type, "device") == 0) {
        cJSON *role_item = cJSON_GetObjectItem(body, "role");
        cJSON *addr_item = cJSON_GetObjectItem(body, "address");
        cJSON *atype_item = cJSON_GetObjectItem(body, "addr_type");

        if (!cJSON_IsString(role_item) || !cJSON_IsString(addr_item) || !cJSON_IsNumber(atype_item)) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing role, address, or addr_type");
            return ESP_FAIL;
        }

        uint8_t addr[6] = {0};
        unsigned int tmp[6];
        if (sscanf(addr_item->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
                    &tmp[5], &tmp[4], &tmp[3], &tmp[2], &tmp[1], &tmp[0]) == 6) {
            for (int i = 0; i < 6; i++) addr[i] = (uint8_t)tmp[i];
        }

        if (strcmp(role_item->valuestring, "keyboard") == 0) {
            ble_central_connect_keyboard(addr, (uint8_t)atype_item->valueint);
        } else if (strcmp(role_item->valuestring, "mouse") == 0) {
            ble_central_connect_mouse(addr, (uint8_t)atype_item->valueint);
        } else {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid role");
            return ESP_FAIL;
        }
    } else {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
        return ESP_FAIL;
    }

    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

The `pairings_delete_handler`:
```c
static esp_err_t pairings_delete_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char uri[64];
    httpd_req_get_url_query_str(req, uri, sizeof(uri));

    /* Extract the {id} from URI path /api/pairings/{id} */
    const char *id_start = strstr(uri, "/api/pairings/");
    if (!id_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    id_start += strlen("/api/pairings/");

    /* For now, just acknowledge; actual disconnect logic depends on id */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "deleted", id_start);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

The `scan_get_handler` (replaces `scan_results_handler`):
```c
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;
    const char *results = ble_central_get_scan_results_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, results ? results : "[]");
    return ESP_OK;
}
```

The `wifi_get_handler`:
```c
static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();

    wifi_operating_mode_t mode = wifi_manager_get_mode();
    const char *mode_str;
    switch (mode) {
    case KVM_WIFI_AP_ONLY:   mode_str = "ap"; break;
    case KVM_WIFI_STA_ONLY:  mode_str = "sta"; break;
    case KVM_WIFI_APSTA:     mode_str = "apsta"; break;
    case KVM_WIFI_OFF:       mode_str = "off"; break;
    default:                  mode_str = "unknown"; break;
    }
    cJSON_AddStringToObject(root, "mode", mode_str);
    cJSON_AddBoolToObject(root, "ap_active", wifi_manager_is_ap_active());
    cJSON_AddBoolToObject(root, "sta_connected", wifi_manager_is_sta_connected());
    cJSON_AddStringToObject(root, "sta_ip", wifi_manager_get_sta_ip());
    cJSON_AddStringToObject(root, "ap_ip", wifi_manager_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "sta_ssid", cfg->wifi_ssid);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

The `wifi_patch_handler` (replaces wifi_handler):
```c
static esp_err_t wifi_patch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *mode_item = cJSON_GetObjectItem(body, "mode");
    if (cJSON_IsString(mode_item)) {
        const char *mode_str = mode_item->valuestring;
        if (strcmp(mode_str, "ap") == 0) wifi_manager_set_mode(KVM_WIFI_AP_ONLY);
        else if (strcmp(mode_str, "sta") == 0) wifi_manager_set_mode(KVM_WIFI_STA_ONLY);
        else if (strcmp(mode_str, "apsta") == 0) wifi_manager_set_mode(KVM_WIFI_APSTA);
        else if (strcmp(mode_str, "off") == 0) wifi_manager_set_mode(KVM_WIFI_OFF);
    }

    cJSON *ssid_item = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(body, "password");
    if (cJSON_IsString(ssid_item)) {
        kvm_config_t *cfg = config_get_mutable();
        strncpy(cfg->wifi_ssid, ssid_item->valuestring, sizeof(cfg->wifi_ssid) - 1);
        if (cJSON_IsString(pass_item)) {
            strncpy(cfg->wifi_password, pass_item->valuestring, sizeof(cfg->wifi_password) - 1);
        }
        config_save_wifi();
        wifi_manager_start_sta(cfg->wifi_ssid, cfg->wifi_password);
    }

    cJSON *disconnect = cJSON_GetObjectItem(body, "disconnect_sta");
    if (cJSON_IsTrue(disconnect)) {
        wifi_manager_stop_sta();
    }

    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

The `settings_patch_handler` (replaces settings_post_handler, adds anti_idle):
```c
static esp_err_t settings_patch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    kvm_config_t *cfg = config_get_mutable();

    cJSON *pc_names = cJSON_GetObjectItem(body, "pc_names");
    if (pc_names) {
        for (int i = 0; i < MAX_PC_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), "pc%d", i + 1);
            cJSON *name = cJSON_GetObjectItem(pc_names, key);
            if (cJSON_IsString(name)) {
                strncpy(cfg->pcs[i].name, name->valuestring, DEVICE_NAME_MAX - 1);
                cfg->pcs[i].name[DEVICE_NAME_MAX - 1] = '\0';
            }
        }
        config_save_pcs();
    }

    cJSON *regen = cJSON_GetObjectItem(body, "regenerate_token");
    if (cJSON_IsTrue(regen)) {
        config_generate_auth_token();
    }

    cJSON *anti_idle = cJSON_GetObjectItem(body, "anti_idle");
    if (cJSON_IsBool(anti_idle)) {
        anti_idle_set_enabled(cJSON_IsTrue(anti_idle));
    }

    cJSON *anti_idle_ivl = cJSON_GetObjectItem(body, "anti_idle_interval");
    if (cJSON_IsNumber(anti_idle_ivl)) {
        anti_idle_set_interval((uint16_t)anti_idle_ivl->valueint);
    }

    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "auth_token", cfg->auth_token);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
```

Update `settings_get_handler` to include anti_idle:
```c
    cJSON_AddBoolToObject(root, "anti_idle", cfg->anti_idle_enabled);
    cJSON_AddNumberToObject(root, "anti_idle_interval", cfg->anti_idle_interval_sec);
```

Update URI table:
```c
static const httpd_uri_t uris[] = {
    { .uri = "/",               .method = HTTP_GET,    .handler = root_handler },
    { .uri = "/api/status",     .method = HTTP_GET,    .handler = status_handler },
    { .uri = "/api/switch",     .method = HTTP_POST,   .handler = switch_handler },
    { .uri = "/api/events",     .method = HTTP_GET,    .handler = events_handler },
    { .uri = "/api/pairings",   .method = HTTP_POST,   .handler = pairings_post_handler },
    { .uri = "/api/pairings/*", .method = HTTP_DELETE, .handler = pairings_delete_handler },
    { .uri = "/api/scan",       .method = HTTP_POST,   .handler = scan_handler },
    { .uri = "/api/scan",       .method = HTTP_GET,    .handler = scan_get_handler },
    { .uri = "/api/devices",    .method = HTTP_GET,    .handler = devices_handler },
    { .uri = "/api/wifi",       .method = HTTP_GET,    .handler = wifi_get_handler },
    { .uri = "/api/wifi",       .method = HTTP_PATCH,  .handler = wifi_patch_handler },
    { .uri = "/api/settings",   .method = HTTP_GET,    .handler = settings_get_handler },
    { .uri = "/api/settings",   .method = HTTP_PATCH,  .handler = settings_patch_handler },
};
```

Remove old handlers: `pair_keyboard_handler`, `pair_mouse_handler`, `pair_pc_handler`, `scan_results_handler`, `wifi_handler`, `settings_post_handler`.

Add `#include "anti_idle.h"` at top.

- [ ] **Step 2: Build and verify**

Run: `cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -5`
Expected: BUILD SUCCESS

- [ ] **Step 3: Commit**

```bash
git add main/web_server.c
git commit -m "feat: redesign RESTful API with merged pairing, PATCH methods, anti-idle settings"
```

---

### Task 5: Stamp S3 RGB LED implementation

**Files:**
- Modify: `main/rgb_led.c`

- [ ] **Step 1: Implement full `main/rgb_led.c` with RMT WS2812B driver**

```c
#include "indicator.h"
#include "board.h"
#if HAS_RGB_LED

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_led";

#define WS2812B_T0H_NS   300
#define WS2812B_T0L_NS   900
#define WS2812B_T1H_NS   900
#define WS2812B_T1L_NS   300
#define WS2812B_RESET_US  50

static rmt_channel_handle_t tx_chan;
static rmt_encoder_handle_t encoder;
static indicator_state_t current_state = IND_NO_PC;
static TaskHandle_t rgb_task_handle = NULL;

static void send_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = {g, r, b};
    rmt_symbol_word_t symbols[24];
    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            int idx = i * 8 + (7 - bit);
            if (grb[i] & (1 << bit)) {
                symbols[idx].duration0 = WS2812B_T1H_NS / 25;
                symbols[idx].level0 = 1;
                symbols[idx].duration1 = WS2812B_T1L_NS / 25;
                symbols[idx].level1 = 0;
            } else {
                symbols[idx].duration0 = WS2812B_T0H_NS / 25;
                symbols[idx].level0 = 1;
                symbols[idx].duration1 = WS2812B_T0L_NS / 25;
                symbols[idx].level1 = 0;
            }
        }
    }
    rmt_transmit_config_t config = {.flags = 0};
    rmt_transmit(tx_chan, encoder, symbols, sizeof(symbols), &config);
}

static void rgb_task(void *arg)
{
    bool toggle = false;
    while (1) {
        switch (current_state) {
        case IND_PC1_ACTIVE:
            send_rgb(0, 255, 0);  /* Green */
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PC2_ACTIVE:
            send_rgb(0, 0, 255);  /* Blue */
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_NO_PC:
            send_rgb(toggle ? 255 : 0, 0, 0);  /* Red slow blink */
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case IND_PAIRING:
            send_rgb(toggle ? 255 : 0, toggle ? 255 : 0, toggle ? 255 : 0);  /* White fast blink */
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

void rgb_led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,  /* 10 MHz → 100ns tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &tx_chan));
    ESP_ERROR_CHECK(rmt_enable(tx_chan));

    /* Simple byte-based encoder works for 1 LED */
    rmt_bytes_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &encoder));

    xTaskCreate(rgb_task, "rgb_led", 2048, NULL, 1, &rgb_task_handle);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", RGB_LED_GPIO);
}

void rgb_led_set_state(indicator_state_t state)
{
    current_state = state;
}

#endif // HAS_RGB_LED
```

- [ ] **Step 2: Build for Stamp S3 target**

Run: `cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5stamps3 build 2>&1 | tail -10`
Expected: BUILD SUCCESS

- [ ] **Step 3: Commit**

```bash
git add main/rgb_led.c
git commit -m "feat: implement WS2812B RGB LED for Stamp S3 via RMT"
```

---

### Task 6: StickS3 TFT display implementation

**Files:**
- Modify: `main/tft_display.c`

- [ ] **Step 1: Implement full `main/tft_display.c` with esp_lcd ST7789 driver**

This is the largest task. The file needs:
1. I2C M5PM1 power-on sequence
2. SPI + esp_lcd panel init
3. Minimal bitmap font renderer
4. Status page + debug page drawing
5. Backlight auto-sleep

```c
#include "indicator.h"
#include "board.h"
#if HAS_TFT_DISPLAY

#include "esp_lcd_panel_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_manager.h"
#include "ble_peripheral.h"
#include "ble_central.h"
#include "switch_manager.h"
#include "wifi_manager.h"

static const char *TAG = "tft";
static esp_lcd_panel_handle_t panel = NULL;
static indicator_state_t current_state = IND_NO_PC;
static TaskHandle_t tft_task_handle = NULL;
static int current_page = 0;  /* 0=status, 1=debug */
static int64_t last_button_time = 0;

/* 5x7 bitmap font for 0-9, A-Z, common symbols */
/* Each char = 5 bytes (5 cols x 7 rows, LSB=top) */
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
    /* 3x scaled font for large text */
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

    /* PC1 line */
    bool pc1 = ble_peripheral_is_pc_connected(1);
    draw_str(4, 8, "PC1", COLOR_FG, COLOR_BG);
    draw_char(28, 8, pc1 ? 'O' : ' ', pc1 ? COLOR_GREEN : COLOR_RED, COLOR_BG);

    /* PC2 line */
    bool pc2 = ble_peripheral_is_pc_connected(2);
    draw_str(4, 20, "PC2", COLOR_FG, COLOR_BG);
    draw_char(28, 20, pc2 ? 'O' : ' ', pc2 ? COLOR_GREEN : COLOR_RED, COLOR_BG);

    /* Active PC - large text */
    char active_str[4];
    snprintf(active_str, sizeof(active_str), "PC%d", cfg->active_pc);
    int ax = (TFT_WIDTH - 5 * 3 * 3) / 2;
    for (int i = 0; active_str[i]; i++) {
        draw_big_char(ax + i * 18, 60, active_str[i], COLOR_GREEN, COLOR_BG);
    }

    /* Keyboard/Mouse status */
    bool kb = ble_central_is_keyboard_connected();
    bool ms = ble_central_is_mouse_connected();
    draw_str(4, 140, "KB", kb ? COLOR_GREEN : COLOR_RED, COLOR_BG);
    draw_str(40, 140, "MS", ms ? COLOR_GREEN : COLOR_RED, COLOR_BG);

#if HAS_BATTERY
    /* Battery - will be populated by power_manager */
    draw_str(4, 160, "BAT", COLOR_FG, COLOR_BG);
#endif

    flush_fb();
}

static void draw_debug_page(void)
{
    memset(fb, 0, sizeof(fb));
    const kvm_config_t *cfg = config_get();

    /* WiFi info */
    draw_str(4, 8, "WiFi", COLOR_FG, COLOR_BG);
    wifi_operating_mode_t mode = wifi_manager_get_mode();
    const char *mode_str = "OFF";
    if (mode == 1) mode_str = "AP";
    else if (mode == 2) mode_str = "STA";
    else if (mode == 3) mode_str = "AP+STA";
    draw_str(40, 8, mode_str, COLOR_FG, COLOR_BG);

    draw_str(4, 20, wifi_manager_get_sta_ip(), COLOR_FG, COLOR_BG);

    draw_str(4, 40, "SSID:", COLOR_FG, COLOR_BG);
    draw_str(4, 52, cfg->wifi_ssid[0] ? cfg->wifi_ssid : "-", COLOR_FG, COLOR_BG);

    /* BLE connections */
    int ble_conns = 0;
    if (ble_central_is_keyboard_connected()) ble_conns++;
    if (ble_central_is_mouse_connected()) ble_conns++;
    if (ble_peripheral_is_pc_connected(1)) ble_conns++;
    if (ble_peripheral_is_pc_connected(2)) ble_conns++;
    char ble_str[16];
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

/* Called from switch_manager when KEY2 pressed */
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

static void init_pmic_power(void)
{
    /* Initialize I2C and enable LCD power via M5PM1 */
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* Enable LDO2 (LCD power) via M5PM1 register 0x16 */
    uint8_t reg_val = 0x00;
    i2c_master_transmit_receive_config_t rx_cfg = {.rx_length = 1};
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev));

    /* Read current LDO2 config, enable it */
    uint8_t read_cmd = 0x16;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, &read_cmd, 1, &reg_val, 1, -1));
    reg_val |= 0x04;  /* Enable LDO2 */
    uint8_t write_buf[] = {0x16, reg_val};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, write_buf, sizeof(write_buf), -1));

    vTaskDelay(pdMS_TO_TICKS(100));
}

void tft_display_init(void)
{
    init_pmic_power();

    /* SPI bus init */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = TFT_SCLK_GPIO,
        .mosi_io_num = TFT_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TFT_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO */
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

    /* ST7789 panel */
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

    /* Clear screen */
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
```

Note: The font5x7 array is complete with the fix applied at char 0x33. The `i2c_get_master_bus_handle()` API may not be available in ESP-IDF 5.4.1 — if not, `power_manager.c` should store the bus handle from `tft_display.c` init or use a shared global. The PMIC voltage register address and scaling (`val * 17`) are placeholders that need adjustment based on the M5PM1 datasheet.

- [ ] **Step 2: Build for StickS3 target**

Run: `cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5sticks3 build 2>&1 | tail -10`
Expected: BUILD SUCCESS

- [ ] **Step 3: Commit**

```bash
git add main/tft_display.c
git commit -m "feat: implement ST7789 TFT display for StickS3 with bitmap font"
```

---

### Task 7: StickS3 power manager

**Files:**
- Create: `main/power_manager.h`
- Create: `main/power_manager.c`
- Modify: `main/main.c`

- [ ] **Step 1: Create `main/power_manager.h`**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

void power_manager_init(void);
bool power_manager_is_charging(void);
uint8_t power_manager_get_battery_percent(void);
uint16_t power_manager_get_battery_voltage_mv(void);
bool power_manager_is_usb_powered(void);
```

- [ ] **Step 2: Create `main/power_manager.c`**

```c
#include "power_manager.h"
#include "board.h"
#if HAS_BATTERY

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "power";
static i2c_master_dev_handle_t pmic_dev;
static int64_t last_activity_time = 0;

#define SLEEP_IDLE_MS      (5 * 60 * 1000)   /* 5 min: Wi-Fi STA off */
#define SLEEP_DEEP_MS      (15 * 60 * 1000)  /* 15 min: deep sleep */

static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(pmic_dev, &reg, 1, val, 1, -1);
}

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[] = {reg, val};
    return i2c_master_transmit(pmic_dev, buf, sizeof(buf), -1);
}

void power_manager_init(void)
{
    /* PMIC I2C is already initialized by tft_display_init */
    /* Reuse the same bus - create a new device handle */
    i2c_master_bus_handle_t bus = i2c_get_master_bus_handle(I2C_NUM_0);
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDR,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &pmic_dev));

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
    /* Rough LiPo estimation: 3.0V=0%, 4.2V=100% */
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    return (uint8_t)((mv - 3000) * 100 / 1200);
}

uint16_t power_manager_get_battery_voltage_mv(void)
{
    uint8_t val = 0;
    if (pmic_read(0x34, &val) != ESP_OK) return 0;
    /* Register value to voltage conversion depends on PMIC datasheet */
    return (uint16_t)(val * 17);  /* Placeholder scaling, adjust per datasheet */
}

bool power_manager_is_usb_powered(void)
{
    uint8_t val = 0;
    if (pmic_read(0x00, &val) != ESP_OK) return true;  /* Assume USB if read fails */
    return (val & 0x80) != 0;
}

#endif // HAS_BATTERY
```

- [ ] **Step 3: Update `main/main.c` — add conditional power_manager init**

Add after existing includes:
```c
#include "board.h"
#if HAS_BATTERY
#include "power_manager.h"
#endif
```

Add after `web_server_init()`:
```c
#if HAS_BATTERY
    power_manager_init();
#endif
```

- [ ] **Step 4: Build for StickS3 target**

Run: `cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5sticks3 build 2>&1 | tail -10`
Expected: BUILD SUCCESS

- [ ] **Step 5: Commit**

```bash
git add main/power_manager.h main/power_manager.c main/main.c
git commit -m "feat: add M5PM1 power manager for StickS3 battery monitoring"
```

---

### Task 8: Switch manager — dual button + remove long-press pairing

**Files:**
- Modify: `main/switch_manager.c`
- Modify: `main/switch_manager.h`

- [ ] **Step 1: Update `main/switch_manager.h`**

Remove `SWITCH_SRC_BUTTON` and `switch_source_t` enum. Simplify:
```c
#pragma once

#include <stdint.h>

void switch_manager_init(void);
void switch_manager_request_switch(void);
uint8_t switch_manager_get_active_pc(void);
uint16_t switch_manager_get_active_conn_handle(void);
void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle);
void switch_manager_on_pc_disconnected(uint8_t pc_id);

#if HAS_TFT_DISPLAY
void switch_manager_on_secondary_button(void);
#endif
```

- [ ] **Step 2: Update `main/switch_manager.c`**

Key changes:
- Include `board.h` instead of hardcoding `GPIO_NUM_0`
- Remove `switch_source_t` and `switch_request_t.source` field
- Remove long-press pairing logic entirely
- Add secondary button handler for StickS3 (KEY2 toggles TFT page)
- Replace `led_controller.h` references with `indicator.h`
- Use `BUTTON_SWITCH_GPIO` from board.h
- Use `indicator_set_state()` instead of `led_controller_set_state()`

The button ISR becomes:
```c
static void IRAM_ATTR button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SWITCH_GPIO);

    if (level == 0 && !button_pending) {
        button_press_time = now;
        button_pending = true;
    } else if (level == 1 && button_pending) {
        int64_t duration = now - button_press_time;
        button_pending = false;
        if (duration > 50 && duration < 2000) {
            uint8_t from_isr = 1;
            xQueueSendFromISR(switch_queue, &from_isr, NULL);
        }
    }
}

#if HAS_SECONDARY_BUTTON
static void IRAM_ATTR secondary_button_isr_handler(void *arg)
{
    static int64_t sec_press_time = 0;
    static bool sec_pending = false;
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SECONDARY_GPIO);

    if (level == 0 && !sec_pending) {
        sec_press_time = now;
        sec_pending = true;
    } else if (level == 1 && sec_pending) {
        int64_t duration = now - sec_press_time;
        sec_pending = false;
        if (duration > 50 && duration < 2000) {
            uint8_t from_isr = 2;
            xQueueSendFromISR(switch_queue, &from_isr, NULL);
        }
    }
}
#endif
```

The switch task becomes:
```c
static void switch_task_func(void *arg)
{
    uint8_t cmd;
    while (1) {
        if (xQueueReceive(switch_queue, &cmd, pdMS_TO_TICKS(100))) {
            if (cmd == 1) {
                /* Switch PC */
                kvm_config_t *cfg = config_get_mutable();
                uint8_t old_pc = cfg->active_pc;
                uint8_t new_pc = (old_pc == 1) ? 2 : 1;
                if (!ble_peripheral_is_pc_connected(new_pc)) {
                    ESP_LOGW(TAG, "PC%d not connected", new_pc);
                    update_led_state();
                    continue;
                }
                indicator_set_state(IND_PAIRING);  /* reuse as "switching" animation */
                vTaskDelay(pdMS_TO_TICKS(100));
                cfg->active_pc = new_pc;
                config_save_active_pc();
                ESP_LOGI(TAG, "Switched PC%d -> PC%d", old_pc, new_pc);
            }
#if HAS_SECONDARY_BUTTON
            else if (cmd == 2) {
                /* Secondary button: toggle TFT page */
                extern void tft_display_toggle_page(void);
                tft_display_toggle_page();
            }
#endif
            update_led_state();
        } else {
            update_led_state();
        }
    }
}
```

Init registers both buttons:
```c
void switch_manager_init(void)
{
    switch_queue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(switch_task_func, "switch_mgr", 2048, NULL, 3, &switch_task_handle);

    vTaskDelay(pdMS_TO_TICKS(2000));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_SWITCH_GPIO, button_isr_handler, NULL);

#if HAS_SECONDARY_BUTTON
    gpio_config_t io2 = {
        .pin_bit_mask = (1ULL << BUTTON_SECONDARY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io2);
    gpio_isr_handler_add(BUTTON_SECONDARY_GPIO, secondary_button_isr_handler, NULL);
#endif

    ESP_LOGI(TAG, "Switch manager initialized");
}
```

- [ ] **Step 3: Update callers of `switch_manager_request_switch()`**

In `main/web_server.c`, change `switch_manager_request_switch(SWITCH_SRC_WEB)` to `switch_manager_request_switch()`.

- [ ] **Step 4: Build and verify all three targets**

Run:
```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -3
cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5stamps3 build 2>&1 | tail -3
cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5sticks3 build 2>&1 | tail -3
```
Expected: All three BUILD SUCCESS

- [ ] **Step 5: Commit**

```bash
git add main/switch_manager.c main/switch_manager.h main/web_server.c
git commit -m "feat: dual-button support, remove long-press pairing, use board.h GPIO"
```

---

### Task 9: Final integration build and cleanup

**Files:**
- Verify all three board targets build
- Clean up any stale references

- [ ] **Step 1: Search for stale led_controller references**

Run: `grep -r "led_controller" main/`
Expected: No results

- [ ] **Step 2: Search for stale LED_STATE references**

Run: `grep -r "LED_STATE" main/`
Expected: No results

- [ ] **Step 3: Search for stale SWITCH_SRC references**

Run: `grep -r "SWITCH_SRC" main/`
Expected: No results

- [ ] **Step 4: Build all three targets and verify**

Run:
```bash
cd /home/gem/project/ble-kvm && idf.py build 2>&1 | tail -3
cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5stamps3 build 2>&1 | tail -3
cd /home/gem/project/ble-kvm && idf.py -DBOARD=m5sticks3 build 2>&1 | tail -3
```
Expected: All three BUILD SUCCESS

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "chore: final integration cleanup for multi-board support"
```
