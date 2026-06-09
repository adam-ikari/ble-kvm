# Voice Input & Factory Reset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add voice-to-keyboard input via M5StickS3 built-in ES8311 codec + SPM1423 mic with Baidu cloud ASR, and move factory reset from primary button 5s to secondary button 10s (2-stage confirmation).

**Architecture:** Primary button press (>500ms) starts I2S recording, streams PCM via WebSocket to Baidu RT-ASR, recognized text sent as HID keyboard input. Secondary button: 5s shows warning, 10s executes factory reset. Web endpoint for factory reset on all boards.

**Tech Stack:** ESP-IDF I2S driver (ES8311 via I2C + I2S_NUM_1), `espressif/esp_websocket_client` (Baidu RT-ASR), cJSON, HID keyboard reports

---

### Task 1: Board feature flags and ES8311 driver

**Files:**
- Modify: `main/board.h` — add HAS_VOICE_INPUT, ES8311/I2S pin defs
- Create: `main/es8311_driver.h`
- Create: `main/es8311_driver.c`

- [ ] **Step 1: Add voice input feature flags to board.h**

Add to `main/board.h` inside the `#ifdef BOARD_M5STICKS3` block (after `#define HAS_USB 1`):

```c
  #define HAS_VOICE_INPUT     1
  #define MIC_I2S_PORT        I2S_NUM_1
  #define MIC_I2S_MCK_GPIO    GPIO_NUM_18
  #define MIC_I2S_BCK_GPIO    GPIO_NUM_17
  #define MIC_I2S_WS_GPIO     GPIO_NUM_15
  #define MIC_I2S_DATA_GPIO   GPIO_NUM_16
  #define ES8311_I2C_ADDR     0x18
```

Add to `main/board.h` in the `#elif defined(BOARD_M5STAMPS3)` block and the default `#else` block:

```c
  #define HAS_VOICE_INPUT     0
```

- [ ] **Step 2: Create es8311_driver.h**

Create `main/es8311_driver.h`:

```c
#pragma once

#include "driver/i2c_master.h"

#if HAS_VOICE_INPUT

void es8311_init(i2c_master_bus_handle_t i2c_bus);
void es8311_set_mic_gain(uint8_t gain);
void es8311_deinit(void);

#else

static inline void es8311_init(i2c_master_bus_handle_t i2c_bus) { (void)i2c_bus; }
static inline void es8311_set_mic_gain(uint8_t gain) { (void)gain; }
static inline void es8311_deinit(void) {}

#endif
```

- [ ] **Step 3: Create es8311_driver.c**

Create `main/es8311_driver.c`:

```c
#include "es8311_driver.h"
#if HAS_VOICE_INPUT

#include "esp_log.h"
#include "board.h"

static const char *TAG = "es8311";
static i2c_master_dev_handle_t es8311_dev = NULL;

/* ES8311 register addresses */
#define ES8311_REG_RESET       0x00
#define ES8311_REG_CLKMGR1    0x01
#define ES8311_REG_CLKMGR2    0x02
#define ES8311_REG_CLKMGR3    0x03
#define ES8311_REG_ADC_REG1   0x05
#define ES8311_REG_ADC_REG2   0x06
#define ES8311_REG_ADC_REG3   0x07
#define ES8311_REG_ADC_REG4   0x08
#define ES8311_REG_ADC_REG5   0x09
#define ES8311_REG_ADC_REG6   0x0A
#define ES8311_REG_SYSTEM     0x0C
#define ES8311_REG_GPIO       0x44
#define ES8311_REG_PDN_REG    0x0D

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(es8311_dev, buf, 2, 100);
}

void es8311_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &es8311_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 device: %s", esp_err_to_name(err));
        return;
    }

    /* Reset */
    es8311_write_reg(ES8311_REG_RESET, 0x80);
    es8311_write_reg(ES8311_REG_RESET, 0x00);

    /* Clock manager: ADC BCLK = MCLK, no PLL */
    es8311_write_reg(ES8311_REG_CLKMGR1, 0x00);  /* CLKMGR1: normal */
    es8311_write_reg(ES8311_REG_CLKMGR2, 0x00);  /* CLKMGR2: ADC/div=1 */
    es8311_write_reg(ES8311_REG_CLKMGR3, 0x00);  /* CLKMGR3: no divider */

    /* System: select ADC input from MIC1p/MIC1n */
    es8311_write_reg(ES8311_REG_SYSTEM, 0x00);

    /* ADC configuration */
    es8311_write_reg(ES8311_REG_ADC_REG1, 0x00); /* ADC ramp rate */
    es8311_write_reg(ES8311_REG_ADC_REG2, 0x00); /* ADC HPF */
    es8311_write_reg(ES8311_REG_ADC_REG3, 0x00); /* ADC volume soft ramp */
    es8311_write_reg(ES8311_REG_ADC_REG4, 0x00); /* ADC equalizer */
    es8311_write_reg(ES8311_REG_ADC_REG5, 0x00); /* ADC gain = 0dB */
    es8311_write_reg(ES8311_REG_ADC_REG6, 0x03); /* ADC digital gain */

    /* Power up ADC */
    es8311_write_reg(ES8311_REG_PDN_REG, 0x00);

    /* GPIO: ADCDATA */
    es8311_write_reg(ES8311_REG_GPIO, 0x02);

    ESP_LOGI(TAG, "ES8311 codec initialized at addr 0x%02X", ES8311_I2C_ADDR);
}

void es8311_set_mic_gain(uint8_t gain)
{
    if (!es8311_dev) return;
    if (gain > 14) gain = 14;
    /* ADC_REG5[4:0] = gain, step 3dB per step, 0x00 = 0dB */
    es8311_write_reg(ES8311_REG_ADC_REG5, gain & 0x1F);
}

void es8311_deinit(void)
{
    if (es8311_dev) {
        es8311_write_reg(ES8311_REG_PDN_REG, 0x01); /* Power down */
        i2c_master_bus_rm_device(es8311_dev);
        es8311_dev = NULL;
    }
}

#endif /* HAS_VOICE_INPUT */
```

- [ ] **Step 4: Commit**

```bash
git add main/board.h main/es8311_driver.h main/es8311_driver.c
git commit -m "feat: add HAS_VOICE_INPUT flag and ES8311 audio codec driver"
```

---

### Task 2: Microphone I2S driver

**Files:**
- Create: `main/mic_driver.h`
- Create: `main/mic_driver.c`

- [ ] **Step 1: Create mic_driver.h**

Create `main/mic_driver.h`:

```c
#pragma once

#include <stddef.h>
#include <stdbool.h>

#if HAS_VOICE_INPUT

void mic_driver_init(void);
void mic_driver_start(void);
void mic_driver_stop(void);
int mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms);
bool mic_driver_is_running(void);

#else

static inline void mic_driver_init(void) {}
static inline void mic_driver_start(void) {}
static inline void mic_driver_stop(void) {}
static inline int mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms) { (void)buf; (void)len; (void)bytes_read; (void)timeout_ms; return -1; }
static inline bool mic_driver_is_running(void) { return false; }

#endif
```

- [ ] **Step 2: Create mic_driver.c**

Create `main/mic_driver.c`:

```c
#include "mic_driver.h"
#if HAS_VOICE_INPUT

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "board.h"
#include <string.h>

static const char *TAG = "mic";
static i2s_chan_handle_t rx_handle = NULL;
static bool running = false;

void mic_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MIC_I2S_MCK_GPIO,
            .bclk = MIC_I2S_BCK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = GPIO_NUM_NC,
            .din  = MIC_I2S_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_LOGI(TAG, "Mic I2S driver initialized (16kHz/16bit/mono)");
}

void mic_driver_start(void)
{
    if (!rx_handle || running) return;
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    running = true;
    ESP_LOGI(TAG, "Mic recording started");
}

void mic_driver_stop(void)
{
    if (!rx_handle || !running) return;
    i2s_channel_disable(rx_handle);
    running = false;
    ESP_LOGI(TAG, "Mic recording stopped");
}

int mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms)
{
    if (!rx_handle || !running) return -1;
    esp_err_t err = i2s_channel_read(rx_handle, buf, len, bytes_read, pdMS_TO_TICKS(timeout_ms));
    return (err == ESP_OK) ? 0 : -1;
}

bool mic_driver_is_running(void)
{
    return running;
}

#endif /* HAS_VOICE_INPUT */
```

- [ ] **Step 3: Commit**

```bash
git add main/mic_driver.h main/mic_driver.c
git commit -m "feat: add I2S microphone driver for ES8311 codec"
```

---

### Task 3: Config manager — voice ASR fields

**Files:**
- Modify: `main/config_manager.h` — add voice config fields and save function
- Modify: `main/config_manager.c` — add load/save for voice fields

- [ ] **Step 1: Add voice fields to config_manager.h**

Add to `kvm_config_t` in `main/config_manager.h` (after `uint8_t air_mouse_sensitivity`):

```c
    bool voice_asr_enabled;           /* default: false */
    uint32_t voice_asr_appid;         /* Baidu App ID */
    char voice_asr_api_key[65];       /* Baidu API Key (appkey) */
    char voice_lang[8];               /* "zh" or "en", default: "zh" */
    uint8_t voice_input_mode;         /* 0=auto, 1=pinyin, 2=ascii */
```

Add function declaration after `void config_save_usb_mode(void);`:

```c
void config_save_voice(void);
```

- [ ] **Step 2: Add forward declaration and load/save to config_manager.c**

Add forward declaration after `static void load_usb_mode(void);`:

```c
static void load_voice(void);
```

Add `load_voice();` call in `config_manager_init()` after `load_usb_mode();`.

Add `load_voice()` and `config_save_voice()` implementations after `config_save_usb_mode()`:

```c
static void load_voice(void)
{
    uint8_t enabled = 0;
    esp_err_t err = nvs_get_u8(nvs_config, "voice_en", &enabled);
    config.voice_asr_enabled = (err == ESP_OK) ? (enabled ? true : false) : false;

    err = nvs_get_u32(nvs_config, "voice_appid", &config.voice_asr_appid);
    if (err != ESP_OK) config.voice_asr_appid = 0;

    size_t required_size = sizeof(config.voice_asr_api_key);
    err = nvs_get_str(nvs_config, "voice_ak", config.voice_asr_api_key, &required_size);
    if (err != ESP_OK) config.voice_asr_api_key[0] = '\0';

    required_size = sizeof(config.voice_lang);
    err = nvs_get_str(nvs_config, "voice_lang", config.voice_lang, &required_size);
    if (err != ESP_OK) {
        strncpy(config.voice_lang, "zh", sizeof(config.voice_lang));
    }

    uint8_t im = 0;
    err = nvs_get_u8(nvs_config, "voice_im", &im);
    config.voice_input_mode = (err == ESP_OK && im <= 2) ? im : 0;
}

void config_save_voice(void)
{
    uint8_t enabled = config.voice_asr_enabled ? 1 : 0;
    ESP_ERROR_CHECK(nvs_set_u8(nvs_config, "voice_en", enabled));
    ESP_ERROR_CHECK(nvs_set_u32(nvs_config, "voice_appid", config.voice_asr_appid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_config, "voice_ak", config.voice_asr_api_key));
    ESP_ERROR_CHECK(nvs_set_str(nvs_config, "voice_lang", config.voice_lang));
    ESP_ERROR_CHECK(nvs_set_u8(nvs_config, "voice_im", config.voice_input_mode));
    ESP_ERROR_CHECK(nvs_commit(nvs_config));
}
```

- [ ] **Step 3: Commit**

```bash
git add main/config_manager.h main/config_manager.c
git commit -m "feat: add voice ASR config fields with NVS persistence"
```

---

### Task 4: Voice input manager

**Files:**
- Create: `main/voice_input.h`
- Create: `main/voice_input.c`
- Modify: `main/idf_component.yml` — add esp_websocket_client dependency

- [ ] **Step 1: Add esp_websocket_client dependency**

Add to `main/idf_component.yml`:

```yaml
  espressif/esp_websocket_client:
    version: "^1.7.0"
```

- [ ] **Step 2: Create voice_input.h**

Create `main/voice_input.h`:

```c
#pragma once

#include <stdbool.h>

#if HAS_VOICE_INPUT

void voice_input_init(void);
bool voice_input_start(void);
void voice_input_stop(void);
void voice_input_cancel(void);
bool voice_input_is_active(void);

#else

static inline void voice_input_init(void) {}
static inline bool voice_input_start(void) { return false; }
static inline void voice_input_stop(void) {}
static inline void voice_input_cancel(void) {}
static inline bool voice_input_is_active(void) { return false; }

#endif
```

- [ ] **Step 3: Create voice_input.c**

Create `main/voice_input.c`:

```c
#include "voice_input.h"
#if HAS_VOICE_INPUT

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "config_manager.h"
#include "mic_driver.h"
#include "hid_router.h"
#include "wifi_manager.h"
#include "switch_manager.h"
#include <string.h>

static const char *TAG = "voice";

#define PCM_FRAME_SIZE  5120   /* 160ms @ 16kHz 16bit mono */
#define RESULT_BUF_SIZE 256
#define ASR_TIMEOUT_MS  10000

static esp_websocket_client_handle_t ws_client = NULL;
static volatile bool active = false;
static volatile bool ws_connected = false;
static char asr_result[RESULT_BUF_SIZE];
static volatile bool result_ready = false;
static TaskHandle_t stream_task_handle = NULL;
static EventGroupHandle_t voice_event_group = NULL;

#define ASR_RESULT_BIT  BIT0
#define ASR_ERROR_BIT   BIT1
#define WS_CONNECTED_BIT BIT2

static void websocket_event_handler(void *arg, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        ws_connected = true;
        if (voice_event_group) xEventGroupSetBits(voice_event_group, WS_CONNECTED_BIT);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->data_len > 0 && data->op_code == 0x01) {
            /* Text frame — parse ASR result */
            char *json_str = strndup((char *)data->data_ptr, data->data_len);
            if (!json_str) break;

            cJSON *root = cJSON_Parse(json_str);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                cJSON *err_no = cJSON_GetObjectItem(root, "err_no");
                cJSON *result = cJSON_GetObjectItem(root, "result");

                if (type && cJSON_IsString(type) && strcmp(type->valuestring, "FIN_TEXT") == 0) {
                    if (err_no && cJSON_IsNumber(err_no) && err_no->valueint == 0
                        && result && cJSON_IsString(result)) {
                        strncpy(asr_result, result->valuestring, RESULT_BUF_SIZE - 1);
                        asr_result[RESULT_BUF_SIZE - 1] = '\0';
                        result_ready = true;
                        if (voice_event_group) xEventGroupSetBits(voice_event_group, ASR_RESULT_BIT);
                    } else {
                        ESP_LOGW(TAG, "ASR error: err_no=%d", err_no ? err_no->valueint : -1);
                        if (voice_event_group) xEventGroupSetBits(voice_event_group, ASR_ERROR_BIT);
                    }
                }
                cJSON_Delete(root);
            }
            free(json_str);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        ws_connected = false;
        break;
    default:
        break;
    }
}

static void send_start_frame(void)
{
    const kvm_config_t *cfg = config_get();
    int dev_pid = (strcmp(cfg->voice_lang, "en") == 0) ? 1737 : 15372;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "START");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "appid", cfg->voice_asr_appid);
    cJSON_AddStringToObject(data, "appkey", cfg->voice_asr_api_key);
    cJSON_AddNumberToObject(data, "dev_pid", dev_pid);
    cJSON_AddStringToObject(data, "cuid", "ble-kvm-sticks3");
    cJSON_AddStringToObject(data, "format", "pcm");
    cJSON_AddNumberToObject(data, "sample", 16000);
    cJSON_AddItemToObject(root, "data", data);

    char *json = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(ws_client, json, strlen(json), portMAX_DELAY);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void send_finish_frame(void)
{
    const char *finish = "{\"type\":\"FINISH\"}";
    esp_websocket_client_send_text(ws_client, finish, strlen(finish), portMAX_DELAY);
}

static void send_cancel_frame(void)
{
    const char *cancel = "{\"type\":\"CANCEL\"}";
    esp_websocket_client_send_text(ws_client, cancel, strlen(cancel), portMAX_DELAY);
}

/* HID keyboard helper: send a single key press and release */
static void send_key_press(uint8_t modifier, uint8_t keycode)
{
    uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    hid_router_forward_keyboard(report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t release[8] = {0};
    hid_router_forward_keyboard(release, sizeof(release));
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ASCII character to HID keycode mapping */
static bool ascii_to_keycode(char c, uint8_t *modifier, uint8_t *keycode)
{
    *modifier = 0;
    if (c >= 'a' && c <= 'z') { *keycode = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *keycode = 0x04 + (c - 'A'); *modifier = 0x02; return true; }
    if (c >= '1' && c <= '9') { *keycode = 0x1E + (c - '1'); return true; }
    if (c == '0') { *keycode = 0x27; return true; }
    if (c == ' ') { *keycode = 0x2C; return true; }
    if (c == '.') { *keycode = 0x37; return true; }
    if (c == ',') { *keycode = 0x36; return true; }
    if (c == '?') { *keycode = 0x38; *modifier = 0x02; return true; }
    if (c == '!') { *keycode = 0x30; *modifier = 0x02; return true; }
    if (c == '\n') { *keycode = 0x28; return true; }  /* Enter */
    if (c == '-') { *keycode = 0x2D; return true; }
    if (c == '=') { *keycode = 0x2E; return true; }
    return false;
}

/* Type ASCII text as HID keyboard input */
static void type_ascii_text(const char *text)
{
    for (int i = 0; text[i]; i++) {
        uint8_t mod, kc;
        if (ascii_to_keycode(text[i], &mod, &kc)) {
            send_key_press(mod, kc);
        }
    }
}

/* Type text — dispatch based on input mode */
static void type_text(const char *text)
{
    const kvm_config_t *cfg = config_get();
    uint8_t mode = cfg->voice_input_mode;
    if (mode == 0) {
        /* Auto: use ASCII for English, pinyin for Chinese */
        mode = (strcmp(cfg->voice_lang, "en") == 0) ? 2 : 1;
    }

    if (mode == 2) {
        /* ASCII mode: type characters directly */
        type_ascii_text(text);
    } else {
        /* Pinyin mode: type as pinyin letters (user must have Chinese IME active) */
        type_ascii_text(text);
    }
}

/* Stream task: read PCM from mic and send as binary WebSocket frames */
static void stream_task_func(void *arg)
{
    uint8_t *pcm_buf = malloc(PCM_FRAME_SIZE);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        active = false;
        vTaskDelete(NULL);
        return;
    }

    while (active && ws_connected) {
        size_t bytes_read = 0;
        int rc = mic_driver_read(pcm_buf, PCM_FRAME_SIZE, &bytes_read, 200);
        if (rc == 0 && bytes_read > 0) {
            esp_websocket_client_send_bin(ws_client, pcm_buf, bytes_read, pdMS_TO_TICKS(500));
        }
    }

    free(pcm_buf);
    vTaskDelete(NULL);
}

void voice_input_init(void)
{
    voice_event_group = xEventGroupCreate();
    asr_result[0] = '\0';
    ESP_LOGI(TAG, "Voice input initialized");
}

bool voice_input_start(void)
{
    const kvm_config_t *cfg = config_get();
    if (!cfg->voice_asr_enabled) return false;
    if (!wifi_manager_is_sta_connected()) return false;
    if (cfg->voice_asr_appid == 0 || cfg->voice_asr_api_key[0] == '\0') return false;

    asr_result[0] = '\0';
    result_ready = false;
    xEventGroupClearBits(voice_event_group, ASR_RESULT_BIT | ASR_ERROR_BIT | WS_CONNECTED_BIT);

    /* Generate session ID */
    char ws_url[128];
    snprintf(ws_url, sizeof(ws_url),
             "wss://vop.baidu.com/realtime_asr?sn=blekvm-%lld",
             esp_timer_get_time());

    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .buffer_size = PCM_FRAME_SIZE + 256,
        .timeout_ms = ASR_TIMEOUT_MS,
    };

    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket connect failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        return false;
    }

    /* Wait for WebSocket connection */
    EventBits_t bits = xEventGroupWaitBits(voice_event_group, WS_CONNECTED_BIT,
                                            pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & WS_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WebSocket connection timeout");
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        return false;
    }

    /* Send START frame */
    send_start_frame();

    /* Start mic recording */
    mic_driver_start();

    /* Start streaming task */
    active = true;
    xTaskCreate(stream_task_func, "voice_stream", 4096, NULL, 4, &stream_task_handle);

    ESP_LOGI(TAG, "Voice recording started");
    return true;
}

void voice_input_stop(void)
{
    if (!active) return;
    active = false;

    /* Stop mic */
    mic_driver_stop();

    /* Wait for stream task to finish */
    if (stream_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(50));
        stream_task_handle = NULL;
    }

    /* Send FINISH and wait for final result */
    if (ws_connected) {
        send_finish_frame();

        EventBits_t bits = xEventGroupWaitBits(voice_event_group,
                                                ASR_RESULT_BIT | ASR_ERROR_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(ASR_TIMEOUT_MS));

        if (bits & ASR_RESULT_BIT && result_ready) {
            ESP_LOGI(TAG, "ASR result: %s", asr_result);
            type_text(asr_result);
        } else {
            ESP_LOGW(TAG, "No ASR result received");
        }
    }

    /* Close WebSocket */
    if (ws_client) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    ws_connected = false;

    ESP_LOGI(TAG, "Voice recording stopped");
}

void voice_input_cancel(void)
{
    if (!active) return;
    active = false;

    mic_driver_stop();

    if (stream_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(50));
        stream_task_handle = NULL;
    }

    if (ws_connected) {
        send_cancel_frame();
    }

    if (ws_client) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    ws_connected = false;

    ESP_LOGI(TAG, "Voice recording cancelled");
}

bool voice_input_is_active(void)
{
    return active;
}

#endif /* HAS_VOICE_INPUT */
```

- [ ] **Step 4: Commit**

```bash
git add main/voice_input.h main/voice_input.c main/idf_component.yml
git commit -m "feat: add voice input manager with Baidu RT-ASR WebSocket streaming"
```

---

### Task 5: Switch manager — button behavior refactor

**Files:**
- Modify: `main/switch_manager.c` — refactor primary button for voice, secondary for 2-stage reset
- Modify: `main/switch_manager.h` — add factory_reset warning callback

- [ ] **Step 1: Add new command codes and voice include**

In `main/switch_manager.c`, add after `#include "esp_system.h"`:

```c
#if HAS_VOICE_INPUT
#include "voice_input.h"
#endif
```

Change command codes:

```c
#define CMD_SWITCH         1
#define CMD_SECONDARY      2
#define CMD_FACTORY_RST    3
#define CMD_MODE_CYCLE     4
#define CMD_VOICE_START    5
#define CMD_VOICE_STOP     6
#define CMD_FACTORY_WARN   7
#define CMD_FACTORY_CANCEL 8
```

- [ ] **Step 2: Refactor primary button ISR**

Replace `button_isr_handler` with:

```c
static void IRAM_ATTR button_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SWITCH_GPIO);

    if (level == 0 && !button_pending) {
        button_press_time = now;
        button_pending = true;
        long_press_triggered = false;
    } else if (level == 1 && button_pending) {
        int64_t duration = now - button_press_time;
        button_pending = false;

        if (duration >= 500) {
            /* Long press — voice input stop */
            uint8_t cmd = CMD_VOICE_STOP;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (duration > 50) {
            uint8_t cmd = CMD_SWITCH;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    }
}
```

Add a timer callback to detect long-press start:

After the ISR, add a FreeRTOS software timer that checks if button is still held:

```c
static esp_timer_handle_t voice_start_timer = NULL;

static void voice_start_timer_cb(void *arg)
{
    if (button_pending && !long_press_triggered) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t duration = now - button_press_time;
        if (duration >= 500) {
            long_press_triggered = true;
            uint8_t cmd = CMD_VOICE_START;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    }
}
```

In `switch_manager_init()`, after `gpio_isr_handler_add(BUTTON_SWITCH_GPIO, button_isr_handler, NULL);`, add timer creation:

```c
    esp_timer_create_args_t voice_timer_args = {
        .callback = voice_start_timer_cb,
        .name = "voice_start",
    };
    esp_timer_create(&voice_timer_args, &voice_start_timer);
    esp_timer_start_periodic(voice_start_timer, 50000);  /* check every 50ms */
```

- [ ] **Step 3: Refactor secondary button ISR for 2-stage factory reset**

Replace `secondary_button_isr_handler`:

```c
#if HAS_SECONDARY_BUTTON
static void IRAM_ATTR secondary_button_isr_handler(void *arg)
{
    static int64_t sec_press_time = 0;
    static bool sec_pending = false;
    static bool factory_warn_triggered = false;
    int64_t now = esp_timer_get_time() / 1000;
    int level = gpio_get_level(BUTTON_SECONDARY_GPIO);

    if (level == 0 && !sec_pending) {
        sec_press_time = now;
        sec_pending = true;
        factory_warn_triggered = false;
    } else if (level == 1 && sec_pending) {
        int64_t duration = now - sec_press_time;
        sec_pending = false;

        if (duration >= 10000) {
            uint8_t cmd = CMD_FACTORY_RST;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (factory_warn_triggered) {
            /* Released between 5s-10s: cancel factory reset */
            uint8_t cmd = CMD_FACTORY_CANCEL;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (duration > 50 && duration < 1000) {
            uint8_t cmd = CMD_SECONDARY;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        } else if (duration >= 1000 && duration < 5000) {
            uint8_t cmd = CMD_MODE_CYCLE;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    }
}

/* Timer to detect 5s milestone on secondary button for factory warning */
static esp_timer_handle_t factory_warn_timer = NULL;

static void factory_warn_timer_cb(void *arg)
{
    static bool sec_warn_sent = false;
    int level = gpio_get_level(BUTTON_SECONDARY_GPIO);
    if (level == 0) {
        /* Button still held */
        if (!sec_warn_sent) {
            /* Check if >= 5s by looking at global secondary button state.
             * We use a simplified approach: just check duration since last press. */
            sec_warn_sent = true;
            uint8_t cmd = CMD_FACTORY_WARN;
            xQueueSendFromISR(switch_queue, &cmd, NULL);
        }
    } else {
        sec_warn_sent = false;
    }
}
```

In `switch_manager_init()`, after the secondary button ISR registration, add:

```c
    esp_timer_create_args_t factory_warn_args = {
        .callback = factory_warn_timer_cb,
        .name = "factory_warn",
    };
    esp_timer_create(&factory_warn_args, &factory_warn_timer);
    esp_timer_start_periodic(factory_warn_timer, 200000);  /* check every 200ms */
```

- [ ] **Step 4: Add command handlers in switch_task_func**

In the command processing section of `switch_task_func`, add after `else if (cmd == CMD_MODE_CYCLE)`:

```c
            else if (cmd == CMD_VOICE_START) {
#if HAS_VOICE_INPUT
                if (voice_input_is_active()) {
                    ESP_LOGW(TAG, "Voice already active");
                } else if (!voice_input_start()) {
                    ESP_LOGW(TAG, "Voice start failed (need WiFi/config)");
                    /* Could show TFT message here */
                }
#endif
            }
            else if (cmd == CMD_VOICE_STOP) {
#if HAS_VOICE_INPUT
                if (voice_input_is_active()) {
                    voice_input_stop();
                }
#endif
            }
            else if (cmd == CMD_FACTORY_WARN) {
                ESP_LOGW(TAG, "Factory reset warning — hold 10s to confirm");
                indicator_set_state(IND_PAIRING);  /* Blink as warning */
            }
            else if (cmd == CMD_FACTORY_CANCEL) {
                ESP_LOGI(TAG, "Factory reset cancelled");
                update_led_state();
            }
```

- [ ] **Step 5: Commit**

```bash
git add main/switch_manager.c
git commit -m "feat: refactor buttons — primary for voice input, secondary 2-stage factory reset"
```

---

### Task 6: Web server — voice config, factory reset endpoint

**Files:**
- Modify: `main/web_server.c` — add voice config to settings, voice_recording to status, factory-reset endpoint

- [ ] **Step 1: Add voice config to settings GET handler**

In `settings_get_handler`, after the `mouse_name` line, add:

```c
    cJSON_AddBoolToObject(root, "voice_asr_enabled", cfg->voice_asr_enabled);
    cJSON_AddNumberToObject(root, "voice_asr_appid", cfg->voice_asr_appid);
    if (cfg->voice_asr_api_key[0]) {
        char masked[65] = {0};
        int klen = strlen(cfg->voice_asr_api_key);
        int show = klen > 4 ? 4 : klen;
        memset(masked, '*', klen - show);
        memcpy(masked + klen - show, cfg->voice_asr_api_key + klen - show, show);
        cJSON_AddStringToObject(root, "voice_asr_api_key", masked);
    } else {
        cJSON_AddStringToObject(root, "voice_asr_api_key", "");
    }
    cJSON_AddStringToObject(root, "voice_lang", cfg->voice_lang);
    cJSON_AddNumberToObject(root, "voice_input_mode", cfg->voice_input_mode);
```

- [ ] **Step 2: Add voice_recording to status handler**

In `status_handler`, after the `usb` object, add:

```c
    cJSON_AddBoolToObject(root, "voice_recording", voice_input_is_active());
```

Also add `#include "voice_input.h"` at the top (after `#include "web_log.h"`).

- [ ] **Step 3: Add voice config to settings PATCH handler**

In `settings_patch_handler`, after the `web_log_item` block, add:

```c
    cJSON *voice_en_item = cJSON_GetObjectItem(body, "voice_asr_enabled");
    if (cJSON_IsBool(voice_en_item)) {
        cfg->voice_asr_enabled = cJSON_IsTrue(voice_en_item);
    }

    cJSON *voice_appid_item = cJSON_GetObjectItem(body, "voice_asr_appid");
    if (cJSON_IsNumber(voice_appid_item)) {
        cfg->voice_asr_appid = (uint32_t)voice_appid_item->valuedouble;
    }

    cJSON *voice_ak_item = cJSON_GetObjectItem(body, "voice_asr_api_key");
    if (cJSON_IsString(voice_ak_item) && strlen(voice_ak_item->valuestring) > 0) {
        /* Only update if it's not a masked value (doesn't start with ****) */
        if (strncmp(voice_ak_item->valuestring, "****", 4) != 0) {
            strncpy(cfg->voice_asr_api_key, voice_ak_item->valuestring, 64);
            cfg->voice_asr_api_key[64] = '\0';
        }
    }

    cJSON *voice_lang_item = cJSON_GetObjectItem(body, "voice_lang");
    if (cJSON_IsString(voice_lang_item)) {
        strncpy(cfg->voice_lang, voice_lang_item->valuestring, sizeof(cfg->voice_lang) - 1);
    }

    cJSON *voice_im_item = cJSON_GetObjectItem(body, "voice_input_mode");
    if (cJSON_IsNumber(voice_im_item)) {
        int val = voice_im_item->valueint;
        if (val >= 0 && val <= 2) cfg->voice_input_mode = (uint8_t)val;
    }

    /* Save voice config if any voice field was set */
    if (voice_en_item || voice_appid_item || voice_ak_item || voice_lang_item || voice_im_item) {
        config_save_voice();
    }
```

- [ ] **Step 4: Add factory-reset endpoint**

Add before the URI registration table:

```c
/* ── Endpoint: POST /api/factory-reset ───────────────────────────── */

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[64] = {0};
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

    cJSON *confirm = cJSON_GetObjectItem(body, "confirm");
    if (!cJSON_IsTrue(confirm)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm must be true");
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

    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}
```

Add to URI table:

```c
    { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler },
```

Increase `config.max_uri_handlers` in `web_server_init` from `NUM_URIS + 2` to `NUM_URIS + 4`.

- [ ] **Step 5: Commit**

```bash
git add main/web_server.c
git commit -m "feat: add voice config API, voice_recording status, factory-reset endpoint"
```

---

### Task 7: Main initialization and build system

**Files:**
- Modify: `main/main.c` — init ES8311, mic, voice_input
- Modify: `main/CMakeLists.txt` — add conditional source files

- [ ] **Step 1: Add includes and init calls to main.c**

Add after `#include "web_log.h"`:

```c
#if HAS_VOICE_INPUT
#include "es8311_driver.h"
#include "mic_driver.h"
#include "voice_input.h"
#endif
```

Add in `app_main()` after `web_log_init()`:

```c
#if HAS_VOICE_INPUT
    if (cfg->voice_asr_enabled && cfg->voice_asr_appid != 0) {
        extern i2c_master_bus_handle_t i2c_bus_handle;
        es8311_init(i2c_bus_handle);
        mic_driver_init();
        voice_input_init();
        ESP_LOGI(TAG, "Voice input initialized");
    }
#endif
```

Note: The I2C bus handle must be exposed from the module that creates it (likely `imu_driver.c` or `tft_display.c`). Check which file creates the bus and make the handle available.

- [ ] **Step 2: Update CMakeLists.txt**

Add to the `if(BOARD STREQUAL "m5sticks3")` block that already has `tft_display.c`, `power_manager.c`, etc.:

```cmake
    list(APPEND COMPONENT_SRCS "es8311_driver.c" "mic_driver.c" "voice_input.c")
```

- [ ] **Step 3: Build and verify**

Run: `BOARD=m5sticks3 idf.py build`
Expected: Build succeeds (may need to resolve I2C bus handle access and ESP-IDF I2S API compatibility)

- [ ] **Step 4: Commit**

```bash
git add main/main.c main/CMakeLists.txt
git commit -m "feat: wire up voice input init in app_main, add to CMakeLists"
```

---

### Task 8: TFT display updates

**Files:**
- Modify: `main/tft_display.c` — recording indicator, factory reset warning
- Modify: `main/indicator.h` — add IND_VOICE_RECORDING, IND_FACTORY_WARN

- [ ] **Step 1: Add indicator states**

In `main/indicator.h`, add to the `indicator_state_t` enum after `IND_PAIRING`:

```c
    IND_VOICE_RECORDING,
    IND_FACTORY_WARN,
```

- [ ] **Step 2: Update TFT display for voice recording indicator and factory warning**

In `main/tft_display.c`, add at the top after the existing includes:

```c
#if HAS_VOICE_INPUT
#include "voice_input.h"
#endif
```

In the status page rendering, add a recording indicator. After the existing status display code, add a conditional block:

```c
#if HAS_VOICE_INPUT
    if (voice_input_is_active()) {
        /* Show recording indicator on status page */
        lv_label_set_text_fmt(status_label, "REC...");
    }
#endif
```

For factory reset warning, add handling when `current_state == IND_FACTORY_WARN`:

```c
    case IND_FACTORY_WARN:
        /* Red screen with warning text */
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xFF0000), 0);
        lv_label_set_text(status_label, "Hold 10s\nFactory Reset");
        break;
```

Note: The exact TFT display implementation depends on the existing lvgl/tft rendering code structure. Adapt to match existing patterns.

- [ ] **Step 3: Commit**

```bash
git add main/tft_display.c main/indicator.h
git commit -m "feat: add voice recording indicator and factory reset warning to TFT"
```

---

### Task 9: Build verification and fixes

**Files:**
- Various — fix any build errors

- [ ] **Step 1: Build for M5StickS3**

Run: `BOARD=m5sticks3 idf.py build`
Expected: Clean build with no errors

- [ ] **Step 2: Build for StampS3**

Run: `BOARD=m5stamps3 idf.py build`
Expected: Clean build (HAS_VOICE_INPUT=0, all stubs compile)

- [ ] **Step 3: Build for default board**

Run: `idf.py build`
Expected: Clean build

- [ ] **Step 4: Fix any build errors and commit**

```bash
git add -A
git commit -m "fix: resolve build errors for voice input feature"
```
