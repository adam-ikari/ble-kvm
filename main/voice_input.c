#include "voice_input.h"
#include "board.h"

#if HAS_VOICE_INPUT

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>

#include "config_manager.h"
#include "mic_driver.h"
#include "hid_router.h"
#include "wifi_manager.h"

static const char *TAG = "voice_input";

/* PCM frame size: 160 ms @ 16 kHz, 16-bit mono = 5120 bytes */
#define PCM_FRAME_BYTES  5120
#define PCM_FRAME_MS     160
#define READ_TIMEOUT_MS  500

/* Maximum length for a single ASR result */
#define ASR_RESULT_MAX   512

/* Timeout waiting for FIN_TEXT after sending FINISH */
#define FINISH_TIMEOUT_MS  5000

/* Task stack and priority */
#define VOICE_TASK_STACK   6144
#define VOICE_TASK_PRIO    4

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    VOICE_IDLE = 0,
    VOICE_CONNECTING,
    VOICE_STREAMING,
    VOICE_FINISHING,
} voice_state_t;

static volatile voice_state_t voice_state = VOICE_IDLE;
static esp_websocket_client_handle_t ws_client = NULL;
static TaskHandle_t voice_task_handle = NULL;
static SemaphoreHandle_t finish_sem = NULL;

/* Buffer for the final recognized text (protected by atomic state) */
static char asr_result[ASR_RESULT_MAX + 1];

/* ------------------------------------------------------------------ */
/* HID keyboard typing helpers                                         */
/* ------------------------------------------------------------------ */

/* Standard USB HID keycodes for US layout (HID Usage Table v1.12) */
#define HID_KEY_A       0x04
#define HID_KEY_1       0x1E
#define HID_KEY_ENTER   0x28
#define HID_KEY_SPACE   0x2C
#define HID_KEY_MINUS   0x2D
#define HID_KEY_EQUAL   0x2E
#define HID_KEY_LBRACE  0x2F
#define HID_KEY_RBRACE  0x30
#define HID_KEY_BKSLASH 0x31
#define HID_KEY_SEMI    0x33
#define HID_KEY_QUOTE   0x34
#define HID_KEY_GRAVE   0x35
#define HID_KEY_COMMA   0x36
#define HID_KEY_PERIOD  0x37
#define HID_KEY_SLASH   0x38

#define HID_MOD_LSHIFT  0x02

/* Send an 8-byte HID keyboard report: [modifier, 0, key1, key2, key3, key4, key5, key6] */
static void send_hid_key(uint8_t modifier, uint8_t keycode)
{
    uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    hid_router_forward_keyboard(report, sizeof(report));
}

/* Send a key-down followed by key-up (all-keys-released) report */
static void type_key(uint8_t modifier, uint8_t keycode)
{
    send_hid_key(modifier, keycode);
    /* Release all keys */
    send_hid_key(0, 0);
    /* Small delay between keystrokes for the host to process */
    vTaskDelay(pdMS_TO_TICKS(8));
}

/* Map an ASCII character to HID modifier + keycode.
 * Returns 0 on success, -1 if the character cannot be typed. */
static int ascii_to_hid(char ch, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    *key = 0;

    if (ch >= 'a' && ch <= 'z') {
        *key = HID_KEY_A + (ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
        *mod = HID_MOD_LSHIFT;
        *key = HID_KEY_A + (ch - 'A');
    } else if (ch >= '1' && ch <= '9') {
        *key = HID_KEY_1 + (ch - '1');
    } else if (ch == '0') {
        *key = HID_KEY_1 + 9;  /* 0 is right after 9 */
    } else {
        /* Punctuation */
        switch (ch) {
        case ' ':  *key = HID_KEY_SPACE;   break;
        case '\n': *key = HID_KEY_ENTER;   break;
        case '-':  *key = HID_KEY_MINUS;   break;
        case '=':  *key = HID_KEY_EQUAL;   break;
        case '[':  *key = HID_KEY_LBRACE;  break;
        case ']':  *key = HID_KEY_RBRACE;  break;
        case '\\': *key = HID_KEY_BKSLASH; break;
        case ';':  *key = HID_KEY_SEMI;    break;
        case '\'': *key = HID_KEY_QUOTE;   break;
        case '`':  *key = HID_KEY_GRAVE;   break;
        case ',':  *key = HID_KEY_COMMA;   break;
        case '.':  *key = HID_KEY_PERIOD;  break;
        case '/':  *key = HID_KEY_SLASH;   break;
        /* Shifted punctuation */
        case '!':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1;      break;
        case '@':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 1;  break;
        case '#':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 2;  break;
        case '$':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 3;  break;
        case '%':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 4;  break;
        case '^':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 5;  break;
        case '&':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 6;  break;
        case '*':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 7;  break;
        case '(':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 8;  break;
        case ')':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_1 + 9;  break;
        case '_':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_MINUS;   break;
        case '+':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_EQUAL;   break;
        case '{':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_LBRACE;  break;
        case '}':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_RBRACE;  break;
        case '|':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_BKSLASH; break;
        case ':':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_SEMI;    break;
        case '"':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_QUOTE;   break;
        case '~':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_GRAVE;   break;
        case '<':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_COMMA;   break;
        case '>':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_PERIOD;  break;
        case '?':  *mod = HID_MOD_LSHIFT; *key = HID_KEY_SLASH;   break;
        case '\t': *key = 0x2B; break; /* HID Tab */
        default:
            return -1;
        }
    }
    return 0;
}

/* Type a UTF-8 string as HID keyboard input.
 * For voice_input_mode == 2 (ascii), non-ASCII bytes are skipped.
 * For voice_input_mode == 0 (auto) or 1 (pinyin), non-ASCII characters
 * are skipped since we cannot type CJK via a standard HID keyboard. */
static void type_text(const char *text, uint8_t voice_input_mode)
{
    (void)voice_input_mode; /* CJK typing not supported over basic HID */

    for (; *text; text++) {
        /* Skip UTF-8 continuation bytes and any non-ASCII character */
        uint8_t ch = (uint8_t)*text;
        if (ch > 0x7F) {
            /* Skip the entire multi-byte UTF-8 sequence */
            while ((*text & 0xC0) == 0x80) text++;
            /* After the loop, text points at the last continuation byte.
             * The for-loop increment will advance past it. */
            continue;
        }

        uint8_t mod, key;
        if (ascii_to_hid(ch, &mod, &key) == 0) {
            type_key(mod, key);
        }
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket event handler                                             */
/* ------------------------------------------------------------------ */

static void ws_event_handler(void *arg,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        voice_state = VOICE_STREAMING;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && data->data_len > 0) {
            /* Null-terminate for parsing */
            char *json = strndup((const char *)data->data_ptr,
                                 data->data_len);
            if (!json) break;

            ESP_LOGD(TAG, "WS text: %s", json);

            cJSON *root = cJSON_Parse(json);
            if (root) {
                cJSON *err_no = cJSON_GetObjectItem(root, "err_no");
                cJSON *type   = cJSON_GetObjectItem(root, "type");
                cJSON *result = cJSON_GetObjectItem(root, "result");

                if (err_no && err_no->valueint != 0) {
                    ESP_LOGE(TAG, "ASR error %d", err_no->valueint);
                } else if (type && cJSON_IsString(type)) {
                    if (strcmp(type->valuestring, "FIN_TEXT") == 0 &&
                        result && cJSON_IsString(result)) {
                        /* Final result received */
                        strncpy(asr_result, result->valuestring,
                                ASR_RESULT_MAX);
                        asr_result[ASR_RESULT_MAX] = '\0';
                        ESP_LOGI(TAG, "ASR result: %s", asr_result);

                        /* Signal the streaming task that we got the result */
                        if (finish_sem) {
                            xSemaphoreGive(finish_sem);
                        }
                    }
                }
                cJSON_Delete(root);
            }
            free(json);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        voice_state = VOICE_IDLE;
        if (finish_sem) xSemaphoreGive(finish_sem);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket closed");
        voice_state = VOICE_IDLE;
        if (finish_sem) xSemaphoreGive(finish_sem);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Voice streaming task                                                */
/* ------------------------------------------------------------------ */

static void voice_task_func(void *arg)
{
    const kvm_config_t *cfg = config_get();
    uint8_t *pcm_buf = malloc(PCM_FRAME_BYTES);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        voice_state = VOICE_IDLE;
        vTaskDelete(NULL);
        return;
    }

    /* ---- Start mic capture ---- */
    mic_driver_start();

    /* ---- Stream PCM frames ---- */
    while (voice_state == VOICE_STREAMING) {
        size_t bytes_read = 0;
        int rc = mic_driver_read(pcm_buf, PCM_FRAME_BYTES,
                                 &bytes_read, READ_TIMEOUT_MS);
        if (rc != 0 || bytes_read == 0) {
            /* Timeout or error — keep trying until state changes */
            continue;
        }

        if (esp_websocket_client_send_bin(ws_client, (const char *)pcm_buf,
                                          bytes_read, pdMS_TO_TICKS(1000)) < 0) {
            ESP_LOGW(TAG, "Failed to send PCM frame");
            break;
        }
    }

    /* ---- Stop mic capture ---- */
    mic_driver_stop();
    free(pcm_buf);

    /* ---- Send FINISH frame (skip if cancelled) ---- */
    if (voice_state != VOICE_IDLE) {
        const char *finish_json = "{\"type\":\"FINISH\"}";
        esp_websocket_client_send_text(ws_client, finish_json,
                                       strlen(finish_json),
                                       pdMS_TO_TICKS(2000));
        voice_state = VOICE_FINISHING;

        /* Wait for FIN_TEXT result or timeout */
        if (xSemaphoreTake(finish_sem, pdMS_TO_TICKS(FINISH_TIMEOUT_MS)) == pdTRUE) {
            /* Type the recognized text if we got a result */
            if (asr_result[0] != '\0') {
                ESP_LOGI(TAG, "Typing: %s", asr_result);
                type_text(asr_result, cfg->voice_input_mode);
            }
        } else {
            ESP_LOGW(TAG, "Timeout waiting for ASR result");
        }
    }

    /* ---- Cleanup ---- */
    if (ws_client) {
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    voice_state = VOICE_IDLE;

    ESP_LOGI(TAG, "Voice session ended");
    vTaskDelete(NULL);
    voice_task_handle = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void voice_input_init(void)
{
    finish_sem = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "Voice input initialized");
}

bool voice_input_start(void)
{
    if (voice_state != VOICE_IDLE) {
        ESP_LOGW(TAG, "Voice session already active (state=%d)", voice_state);
        return false;
    }

    const kvm_config_t *cfg = config_get();

    if (!cfg->voice_asr_enabled) {
        ESP_LOGW(TAG, "Voice ASR is disabled in config");
        return false;
    }

    if (!wifi_manager_is_sta_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, cannot start voice input");
        return false;
    }

    if (cfg->voice_asr_appid == 0 || cfg->voice_asr_api_key[0] == '\0') {
        ESP_LOGW(TAG, "Baidu ASR credentials not configured");
        return false;
    }

    /* Reset result buffer */
    asr_result[0] = '\0';

    /* Determine dev_pid from language setting */
    int dev_pid = 15372; /* Chinese (Mandatin, far-field) */
    if (strcmp(cfg->voice_lang, "en") == 0) {
        dev_pid = 1737;   /* English */
    }

    /* Build WebSocket URL with a simple UUID (using chip ID as base) */
    char ws_url[128];
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(ws_url, sizeof(ws_url),
             "wss://vop.baidu.com/realtime_asr?sn=ble-kvm-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Configure and create WebSocket client */
    const esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = 0,  /* No auto-reconnect */
        .network_timeout_ms = 10000,
    };

    ws_client = esp_websocket_client_init(&ws_cfg);
    if (!ws_client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return false;
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    if (esp_websocket_client_start(ws_client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        return false;
    }

    voice_state = VOICE_CONNECTING;

    /* Send START frame */
    cJSON *start = cJSON_CreateObject();
    cJSON *data  = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "type", "START");
    cJSON_AddItemToObject(start, "data", data);
    cJSON_AddNumberToObject(data, "appid", cfg->voice_asr_appid);
    cJSON_AddStringToObject(data, "appkey", cfg->voice_asr_api_key);
    cJSON_AddNumberToObject(data, "dev_pid", dev_pid);
    cJSON_AddStringToObject(data, "cuid", "ble-kvm-sticks3");
    cJSON_AddStringToObject(data, "format", "pcm");
    cJSON_AddNumberToObject(data, "sample", 16000);

    char *start_json = cJSON_PrintUnformatted(start);
    ESP_LOGI(TAG, "Sending START frame (appid=%lu, dev_pid=%d)",
             (unsigned long)cfg->voice_asr_appid, dev_pid);

    esp_websocket_client_send_text(ws_client, start_json,
                                   strlen(start_json),
                                   pdMS_TO_TICKS(3000));
    free(start_json);
    cJSON_Delete(start);

    /* Launch streaming task */
    xTaskCreate(voice_task_func, "voice_in", VOICE_TASK_STACK,
                NULL, VOICE_TASK_PRIO, &voice_task_handle);
    return true;
}

void voice_input_stop(void)
{
    if (voice_state == VOICE_IDLE) return;

    ESP_LOGI(TAG, "Stopping voice session (state=%d)", voice_state);

    /* Transition state so the streaming loop exits and sends FINISH */
    if (voice_state == VOICE_STREAMING) {
        voice_state = VOICE_FINISHING;
    }

    /* The task will clean up the WebSocket and delete itself */
}

void voice_input_cancel(void)
{
    if (voice_state == VOICE_IDLE) return;

    ESP_LOGI(TAG, "Cancelling voice session");

    /* Send CANCEL frame if WebSocket is still open */
    if (ws_client) {
        const char *cancel_json = "{\"type\":\"CANCEL\"}";
        esp_websocket_client_send_text(ws_client, cancel_json,
                                       strlen(cancel_json),
                                       pdMS_TO_TICKS(1000));
    }

    voice_state = VOICE_IDLE;

    /* Signal the finish semaphore so the task doesn't hang */
    if (finish_sem) {
        xSemaphoreGive(finish_sem);
    }

    /* The task will see IDLE state and clean up */
}

bool voice_input_is_active(void)
{
    return voice_state != VOICE_IDLE;
}

#endif /* HAS_VOICE_INPUT */
