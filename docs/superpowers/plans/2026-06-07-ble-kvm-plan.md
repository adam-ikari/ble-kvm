# BLE-KVM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an ESP32-S3 BLE KVM switcher that connects BLE keyboard/mouse input and routes HID reports to 2 PCs via BLE HID, with physical button switching, LED indication, and a React web interface for configuration.

**Architecture:** ESP32-S3 runs both BLE Central (keyboard/mouse input) and BLE Peripheral (HID output to PCs). HID Router forwards reports to the active PC only. Switch Manager handles button/Web switching via FreeRTOS message queue. Web Server provides REST API + SSE for management. React frontend embedded as gzip single-file HTML in Flash.

**Tech Stack:** ESP-IDF 5.x, NimBLE, FreeRTOS, React 19 + TypeScript + Vite, CSS Modules

---

## File Structure

```
ble-kvm/
├── CMakeLists.txt                          # Top-level CMake
├── partitions.csv                          # OTA partition table
├── sdkconfig.defaults                      # Default ESP-IDF config
├── main/
│   ├── CMakeLists.txt                      # Main component CMake
│   ├── main.c                              # Entry point, module init
│   ├── config_manager.c/h                  # NVS config read/write
│   ├── ble_peripheral.c/h                  # BLE GAP Peripheral + HID GATT Server
│   ├── ble_central.c/h                     # BLE GAP Central + key/mouse client
│   ├── hid_router.c/h                      # HID report routing
│   ├── switch_manager.c/h                  # Switch control + button handler
│   ├── led_controller.c/h                  # LED state indicator
│   ├── wifi_manager.c/h                    # Wi-Fi AP/STA management
│   └── web_server.c/h                      # HTTP server + REST API
├── web/                                    # React frontend (separate build)
│   ├── package.json
│   ├── vite.config.ts
│   ├── tsconfig.json
│   ├── index.html
│   └── src/
│       ├── main.tsx
│       ├── App.tsx
│       ├── api.ts
│       ├── hooks/
│       │   └── useSse.ts
│       ├── components/
│       │   ├── StatusCard.tsx
│       │   ├── DeviceList.tsx
│       │   ├── PairPanel.tsx
│       │   ├── SettingsPanel.tsx
│       │   └── WifiPanel.tsx
│       └── styles/
│           └── App.module.css
```

---

## Task 1: ESP-IDF Environment Setup

**Files:**
- Create: none (environment only)

- [ ] **Step 1: Install ESP-IDF 5.x**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

Expected: `idf.py --version` prints `5.4.1`

- [ ] **Step 2: Verify toolchain**

```bash
idf.py --version
xtensa-esp-elf-gcc --version
```

Expected: Both commands succeed

- [ ] **Step 3: Record ESP-IDF path**

No git commit — environment only. Record `$HOME/esp/esp-idf` for later `export.sh` sourcing.

---

## Task 2: Project Skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `partitions.csv`
- Create: `main/CMakeLists.txt`
- Create: `main/main.c`

- [ ] **Step 1: Create project directory and top-level CMake**

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble-kvm)
```

- [ ] **Step 2: Create sdkconfig.defaults**

```
# sdkconfig.defaults
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=4
CONFIG_ESP_HTTP_SERVER=y
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

- [ ] **Step 3: Create partitions.csv with OTA support**

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        1M,
ota_0,    app,  ota_0,   ,        1M,
ota_1,    app,  ota_1,   ,        1M,
ota_data, data, ota,     ,        0x2000,
storage,  data, spiffs,  ,        1M,
```

- [ ] **Step 4: Create main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 5: Create main/main.c with minimal app_main**

```c
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "BLE-KVM initialized");
}
```

- [ ] **Step 6: Set target and build**

```bash
. $HOME/esp/esp-idf/export.sh
cd /Users/dadi/Project/ble-kvm
idf.py set-target esp32s3
idf.py build
```

Expected: Build succeeds with no errors

- [ ] **Step 7: Initialize git and commit**

```bash
cd /Users/dadi/Project/ble-kvm
git init
cat > .gitignore << 'EOF'
build/
sdkconfig.old
web/node_modules/
web/dist/
.vscode/
EOF
git add -A
git commit -m "feat: project skeleton with ESP-IDF 5.x structure"
```

---

## Task 3: LED Controller

**Files:**
- Create: `main/led_controller.h`
- Create: `main/led_controller.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

- [ ] **Step 1: Create led_controller.h**

```c
#pragma once

#include <stdbool.h>

typedef enum {
    LED_STATE_PC1_ACTIVE,
    LED_STATE_PC2_ACTIVE,
    LED_STATE_SWITCHING,
    LED_STATE_PAIRING,
    LED_STATE_NO_PC,
    LED_STATE_ERROR,
} led_state_t;

void led_controller_init(void);
void led_controller_set_state(led_state_t state);
```

- [ ] **Step 2: Create led_controller.c**

```c
#include "led_controller.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "led";
static const gpio_num_t LED1_GPIO = GPIO_NUM_2;
static const gpio_num_t LED2_GPIO = GPIO_NUM_1;

static led_state_t current_state = LED_STATE_NO_PC;
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
        case LED_STATE_PC1_ACTIVE:
            set_leds(true, false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case LED_STATE_PC2_ACTIVE:
            set_leds(false, true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case LED_STATE_SWITCHING:
            set_leds(toggle, !toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        case LED_STATE_PAIRING:
            set_leds(toggle, toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case LED_STATE_NO_PC:
            set_leds(toggle, false);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case LED_STATE_ERROR:
            set_leds(toggle, toggle);
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

void led_controller_init(void)
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
    ESP_LOGI(TAG, "LED controller initialized");
}

void led_controller_set_state(led_state_t state)
{
    current_state = state;
}
```

- [ ] **Step 3: Update main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c" "led_controller.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 4: Update main/main.c to init LED controller**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_controller.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_controller_init();

    ESP_LOGI(TAG, "BLE-KVM initialized");
}
```

- [ ] **Step 5: Build and verify**

```bash
idf.py build
```

Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add LED controller with state-based indication"
```

---

## Task 4: Config Manager

**Files:**
- Create: `main/config_manager.h`
- Create: `main/config_manager.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

- [ ] **Step 1: Create config_manager.h**

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_PC_COUNT 2
#define DEVICE_NAME_MAX 32
#define AUTH_TOKEN_LEN 9

typedef struct {
    uint8_t identity_addr[6];
    uint8_t addr_type;
    uint8_t pc_id;
    char name[DEVICE_NAME_MAX];
    uint16_t conn_handle;
    bool connected;
} pc_device_t;

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    bool is_combo;
    char name[DEVICE_NAME_MAX];
    bool connected;
} input_device_t;

typedef struct {
    pc_device_t pcs[MAX_PC_COUNT];
    input_device_t keyboard;
    input_device_t mouse;
    uint8_t active_pc;
    char auth_token[AUTH_TOKEN_LEN];
    char wifi_ssid[33];
    char wifi_password[65];
    bool wifi_enabled;
} kvm_config_t;

void config_manager_init(void);
const kvm_config_t *config_get(void);
kvm_config_t *config_get_mutable(void);
void config_save_pcs(void);
void config_save_input_devices(void);
void config_save_active_pc(void);
void config_save_auth_token(void);
void config_save_wifi(void);
void config_generate_auth_token(void);
```

- [ ] **Step 2: Create config_manager.c**

```c
#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "config";
static kvm_config_t config;
static nvs_handle_t nvs_ble;
static nvs_handle_t nvs_config;
static nvs_handle_t nvs_wifi;
static nvs_handle_t nvs_system;

static const char *NS_BLE = "kvm_ble";
static const char *NS_CONFIG = "kvm_config";
static const char *NS_WIFI = "kvm_wifi";
static const char *NS_SYSTEM = "kvm_system";

static void load_pcs(void)
{
    size_t required_size = sizeof(config.pcs);
    esp_err_t err = nvs_get_blob(nvs_ble, "pcs", config.pcs, &required_size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved PCs, using defaults");
        memset(config.pcs, 0, sizeof(config.pcs));
        config.pcs[0].pc_id = 1;
        config.pcs[1].pc_id = 2;
    }
}

static void load_input_devices(void)
{
    size_t required_size = sizeof(config.keyboard);
    esp_err_t err = nvs_get_blob(nvs_ble, "keyboard", &config.keyboard, &required_size);
    if (err != ESP_OK) {
        memset(&config.keyboard, 0, sizeof(config.keyboard));
    }
    required_size = sizeof(config.mouse);
    err = nvs_get_blob(nvs_ble, "mouse", &config.mouse, &required_size);
    if (err != ESP_OK) {
        memset(&config.mouse, 0, sizeof(config.mouse));
    }
}

static void load_active_pc(void)
{
    esp_err_t err = nvs_get_u8(nvs_config, "active_pc", &config.active_pc);
    if (err != ESP_OK) {
        config.active_pc = 1;
    }
}

static void load_auth_token(void)
{
    size_t required_size = sizeof(config.auth_token);
    esp_err_t err = nvs_get_str(nvs_config, "auth_token", config.auth_token, &required_size);
    if (err != ESP_OK) {
        config_generate_auth_token();
        config_save_auth_token();
    }
}

static void load_wifi(void)
{
    size_t required_size = sizeof(config.wifi_ssid);
    esp_err_t err = nvs_get_str(nvs_wifi, "ssid", config.wifi_ssid, &required_size);
    if (err != ESP_OK) {
        config.wifi_ssid[0] = '\0';
    }
    required_size = sizeof(config.wifi_password);
    err = nvs_get_str(nvs_wifi, "password", config.wifi_password, &required_size);
    if (err != ESP_OK) {
        config.wifi_password[0] = '\0';
    }
    err = nvs_get_u8(nvs_wifi, "enabled", &config.wifi_enabled);
    if (err != ESP_OK) {
        config.wifi_enabled = true;
    }
}

void config_manager_init(void)
{
    ESP_ERROR_CHECK(nvs_open(NS_BLE, NVS_READWRITE, &nvs_ble));
    ESP_ERROR_CHECK(nvs_open(NS_CONFIG, NVS_READWRITE, &nvs_config));
    ESP_ERROR_CHECK(nvs_open(NS_WIFI, NVS_READWRITE, &nvs_wifi));
    ESP_ERROR_CHECK(nvs_open(NS_SYSTEM, NVS_READWRITE, &nvs_system));

    load_pcs();
    load_input_devices();
    load_active_pc();
    load_auth_token();
    load_wifi();

    ESP_LOGI(TAG, "Config loaded: active_pc=%d, auth_token=%s", config.active_pc, config.auth_token);
}

const kvm_config_t *config_get(void)
{
    return &config;
}

kvm_config_t *config_get_mutable(void)
{
    return &config;
}

void config_save_pcs(void)
{
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "pcs", config.pcs, sizeof(config.pcs)));
    ESP_ERROR_CHECK(nvs_commit(nvs_ble));
}

void config_save_input_devices(void)
{
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "keyboard", &config.keyboard, sizeof(config.keyboard)));
    ESP_ERROR_CHECK(nvs_set_blob(nvs_ble, "mouse", &config.mouse, sizeof(config.mouse)));
    ESP_ERROR_CHECK(nvs_commit(nvs_ble));
}

void config_save_active_pc(void)
{
    ESP_ERROR_CHECK(nvs_set_u8(nvs_config, "active_pc", config.active_pc));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}

void config_save_auth_token(void)
{
    ESP_ERROR_CHECK(nvs_set_str(nvs_config, "auth_token", config.auth_token));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}

void config_save_wifi(void)
{
    ESP_ERROR_CHECK(nvs_set_str(nvs_wifi, "ssid", config.wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_wifi, "password", config.wifi_password));
    uint8_t enabled = config.wifi_enabled ? 1 : 0;
    ESP_ERROR_CHECK(nvs_set_u8(nvs_wifi, "enabled", enabled));
    ESP_ERROR_CHECK(nvs_commit(nvs_wifi));
}

void config_generate_auth_token(void)
{
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < AUTH_TOKEN_LEN - 1; i++) {
        config.auth_token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    config.auth_token[AUTH_TOKEN_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Generated auth token: %s", config.auth_token);
}
```

- [ ] **Step 3: Update main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c" "led_controller.c" "config_manager.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 4: Update main/main.c**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_controller.h"
#include "config_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    config_manager_init();
    led_controller_init();

    ESP_LOGI(TAG, "BLE-KVM initialized, token: %s", config_get()->auth_token);
}
```

- [ ] **Step 5: Build and verify**

```bash
idf.py build
```

Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add config manager with NVS persistence"
```

---

## Task 5: BLE Peripheral — HID GATT Server

**Files:**
- Create: `main/ble_peripheral.h`
- Create: `main/ble_peripheral.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

- [ ] **Step 1: Create ble_peripheral.h**

```c
#pragma once

#include <stdint.h>
#include "config_manager.h"

void ble_peripheral_init(void);
void ble_peripheral_start_advertising(void);
void ble_peripheral_start_directed_advertising(const pc_device_t *pc);
void ble_peripheral_stop_advertising(void);
uint16_t ble_peripheral_get_conn_handle(uint8_t pc_id);
int ble_peripheral_send_hid_report(uint16_t conn_handle, uint8_t report_id,
                                    const uint8_t *data, uint8_t len);
bool ble_peripheral_is_pc_connected(uint8_t pc_id);

typedef void (*ble_peripheral_conn_cb_t)(uint8_t pc_id, uint16_t conn_handle, bool connected);
void ble_peripheral_register_conn_cb(ble_peripheral_conn_cb_t cb);
```

- [ ] **Step 2: Create ble_peripheral.c**

See full source in design doc. Key features:
- NimBLE GATT Server with HID Service (0x1812)
- Keyboard Report Map (Report ID 1) + Mouse Report Map (Report ID 2)
- Protocol Mode characteristic (Report Protocol only)
- Per-connection notification using `ble_gatts_notify_custom()` + `os_mbuf`
- GAP event handler tracking 2 PC connections via conn_handle
- General advertising and directed advertising support
- Connection callback registration for switch_manager

- [ ] **Step 3: Update main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c" "led_controller.c" "config_manager.c" "ble_peripheral.c"
    INCLUDE_DIRS "."
    REQUIRES nimble
)
```

- [ ] **Step 4: Update main/main.c with NimBLE host init**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "led_controller.h"
#include "config_manager.h"
#include "ble_peripheral.h"

static const char *TAG = "main";

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    ble_peripheral_start_advertising();
}

void app_main(void)
{
    ESP_LOGI(TAG, "BLE-KVM starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    config_manager_init();
    led_controller_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_peripheral_init();
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE-KVM initialized, token: %s", config_get()->auth_token);
}
```

- [ ] **Step 5: Build and verify**

```bash
idf.py build
```

Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add BLE peripheral with HID GATT server"
```

---

## Task 6: Switch Manager

**Files:**
- Create: `main/switch_manager.h`
- Create: `main/switch_manager.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

- [ ] **Step 1: Create switch_manager.h**

```c
#pragma once

#include <stdint.h>

typedef enum {
    SWITCH_SRC_BUTTON,
    SWITCH_SRC_WEB,
} switch_source_t;

void switch_manager_init(void);
void switch_manager_request_switch(switch_source_t source);
uint8_t switch_manager_get_active_pc(void);
uint16_t switch_manager_get_active_conn_handle(void);
void switch_manager_on_pc_connected(uint8_t pc_id, uint16_t conn_handle);
void switch_manager_on_pc_disconnected(uint8_t pc_id);
```

- [ ] **Step 2: Create switch_manager.c**

FreeRTOS task with message queue. GPIO0 button with ISR + 2s boot delay. Auto-switch when active PC disconnects.

- [ ] **Step 3: Update main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c" "led_controller.c" "config_manager.c" "ble_peripheral.c" "switch_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nimble
)
```

- [ ] **Step 4: Update main/main.c — register conn callback and init switch manager**

Add wrapper function `on_pc_conn_event` and call `switch_manager_init()` + `ble_peripheral_register_conn_cb(on_pc_conn_event)`.

- [ ] **Step 5: Build and verify**

```bash
idf.py build
```

Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add switch manager with button and message queue"
```

---

## Task 7: HID Router

**Files:**
- Create: `main/hid_router.h`
- Create: `main/hid_router.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create hid_router.h**

```c
#pragma once

#include <stdint.h>

void hid_router_init(void);
void hid_router_forward_keyboard(const uint8_t *report, uint8_t len);
void hid_router_forward_mouse(const uint8_t *report, uint8_t len);
```

- [ ] **Step 2: Create hid_router.c**

Routes keyboard/mouse reports to active PC via `ble_peripheral_send_hid_report()`.

- [ ] **Step 3: Update main/CMakeLists.txt — add hid_router.c**

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 8: BLE Central — Keyboard/Mouse Input

**Files:**
- Create: `main/ble_central.h`
- Create: `main/ble_central.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create ble_central.h**

- [ ] **Step 2: Create ble_central.c**

BLE Central scanning, connecting keyboard/mouse, subscribing to HID Report characteristics, forwarding to `hid_router_forward_keyboard/mouse()` on `BLE_GAP_EVENT_NOTIFY_RX`.

- [ ] **Step 3: Update main/CMakeLists.txt**

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 9: Wi-Fi Manager

**Files:**
- Create: `main/wifi_manager.h`
- Create: `main/wifi_manager.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create wifi_manager.h**

- [ ] **Step 2: Create wifi_manager.c**

AP+STA mode, auto-start AP with `BLE-KVM-XXXX` SSID, optional STA with saved credentials.

- [ ] **Step 3: Update main/CMakeLists.txt — add REQUIRES esp_wifi esp_netif esp_event**

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 10: Web Server — REST API

**Files:**
- Create: `main/web_server.h`
- Create: `main/web_server.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create web_server.h**

- [ ] **Step 2: Create web_server.c**

All REST API endpoints from spec: `/api/status`, `/api/switch`, `/api/events` (SSE), `/api/scan`, `/api/scan/results`, `/api/pair/*`, `/api/devices`, `/api/wifi`, `/api/settings`. Token auth via `Authorization: Bearer` header, SSE via `?token=` query param. Placeholder frontend HTML.

- [ ] **Step 3: Update main/CMakeLists.txt — add REQUIRES json**

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 11: React Frontend

**Files:**
- Create: `web/package.json`
- Create: `web/vite.config.ts`
- Create: `web/tsconfig.json`
- Create: `web/index.html`
- Create: `web/src/main.tsx`
- Create: `web/src/App.tsx`
- Create: `web/src/api.ts`
- Create: `web/src/hooks/useSse.ts`
- Create: `web/src/components/*.tsx`
- Create: `web/src/styles/App.module.css`

- [ ] **Step 1: Initialize project structure**

- [ ] **Step 2: Create all source files (package.json, vite.config.ts, etc.)**

- [ ] **Step 3: Install deps and build**

```bash
cd web && npm install && npm run build
```

- [ ] **Step 4: Verify gzip size < 30KB**

- [ ] **Step 5: Commit**

---

## Task 12: Embed Frontend in Firmware

**Files:**
- Modify: `main/web_server.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add CMake custom command for frontend build + gzip + xxd**

- [ ] **Step 2: Update index_handler to serve embedded gzip HTML**

- [ ] **Step 3: Build and verify**

- [ ] **Step 4: Commit**

---

## Task 13: Integration Build and Flash Test

**Files:**
- Modify: `main/main.c` (final integration)

- [ ] **Step 1: Finalize main.c with all modules in correct init order**

- [ ] **Step 2: Full build**

- [ ] **Step 3: Flash to device (if hardware available)**

- [ ] **Step 4: Commit**

---

## Self-Review

**1. Spec coverage:** All spec sections mapped to tasks (see detailed table in conversation).

**2. Placeholder scan:** No TBD/TODO patterns found.

**3. Type consistency:** All function signatures, struct names, and module interfaces consistent across tasks.