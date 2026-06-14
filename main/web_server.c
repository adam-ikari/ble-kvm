#include "web_server.h"
#include "config_manager.h"
#include "switch_manager.h"
#include "ble_central.h"
#include "ble_peripheral.h"
#include "wifi_manager.h"
#include "anti_idle.h"
#include "input_mode.h"
#include "services/gap/ble_svc_gap.h"
#if HAS_USB
#include "usb_device.h"
#include "usb_host.h"
#endif
#include "web_log.h"
#include "voice_input.h"
#if HAS_BATTERY
#include "power_manager.h"
#endif
#include "web_dist_gz.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

#define FIRMWARE_VERSION "0.1.0"
#define MAX_SSE_CLIENTS 4
#define SSE_KEEPALIVE_INTERVAL_MS 15000

typedef struct {
    httpd_handle_t hd;
    int fd;
    bool active;
} sse_client_t;

static sse_client_t sse_clients[MAX_SSE_CLIENTS];
static SemaphoreHandle_t sse_mutex = NULL;

/* ── Auth ─────────────────────────────────────────────────────────── */

static void sse_broadcast(const char *event, const char *data);

static bool check_auth(httpd_req_t *req)
{
    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        const kvm_config_t *cfg = config_get();
        if (cfg->auth_token[0] != '\0') {
            char expected[96];
            snprintf(expected, sizeof(expected), "Bearer %s", cfg->auth_token);
            if (strcmp(auth_header, expected) == 0) {
                return true;
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"waiting\",\"message\":\"Double-click the device button to authorize\"}");
    return false;
}

void web_server_grant_auth(void)
{
    const kvm_config_t *cfg = config_get();
    ESP_LOGI(TAG, "Web access granted, token: %s", cfg->auth_token);

    /* Push token to all SSE clients */
    char data[128];
    snprintf(data, sizeof(data), "{\"token\":\"%s\"}", cfg->auth_token);
    sse_broadcast("auth", data);
}

/* ── SSE helpers ──────────────────────────────────────────────────── */

static void sse_send_to_client(sse_client_t *client, const char *event, const char *data)
{
    char pkt[256];
    int len = snprintf(pkt, sizeof(pkt), "event: %s\ndata: %s\n\n", event, data);
    if (len < 0 || len >= (int)sizeof(pkt)) {
        len = (int)sizeof(pkt) - 1;
        pkt[len] = '\0';
    }
    httpd_socket_send(client->hd, client->fd, pkt, len, 0);
}

static void sse_broadcast(const char *event, const char *data)
{
    if (!sse_mutex) return;
    xSemaphoreTake(sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_clients[i].active) {
            sse_send_to_client(&sse_clients[i], event, data);
        }
    }
    xSemaphoreGive(sse_mutex);
}

static void sse_keepalive_cb(void *arg)
{
    xSemaphoreTake(sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_clients[i].active) {
            sse_send_to_client(&sse_clients[i], "keepalive", "");
        }
    }
    xSemaphoreGive(sse_mutex);
}

static esp_err_t sse_add_client(httpd_req_t *req)
{
    xSemaphoreTake(sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (!sse_clients[i].active) {
            sse_clients[i].hd = req->handle;
            sse_clients[i].fd = httpd_req_to_sockfd(req);
            sse_clients[i].active = true;
            xSemaphoreGive(sse_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(sse_mutex);
    return ESP_FAIL;
}

static void sse_remove_client(int fd)
{
    xSemaphoreTake(sse_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_clients[i].active && sse_clients[i].fd == fd) {
            sse_clients[i].active = false;
            break;
        }
    }
    xSemaphoreGive(sse_mutex);
}

/* ── Endpoint: GET / ─────────────────────────────────────────────── */

static esp_err_t root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Root request, sending %u bytes gzip", web_dist_index_html_gz_len);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Send in 4 KB chunks to avoid overflowing the TCP send buffer (16 KB).
     * httpd_resp_send() pushes everything at once and blocks when the buffer
     * is full, causing the client to time out and disconnect (ECONNRESET). */
    esp_err_t err;
    const char *data = (const char *)web_dist_index_html_gz;
    size_t remaining = web_dist_index_html_gz_len;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > 4096) ? 4096 : remaining;
        err = httpd_resp_send_chunk(req, data + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send HTML chunk at offset %u: %s",
                     (unsigned)offset, esp_err_to_name(err));
            return err;
        }
        offset += chunk;
        remaining -= chunk;
    }

    /* Signal end of chunked response */
    err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HTML terminator: %s", esp_err_to_name(err));
    }
    return err;
}

/* ── Endpoint: GET /api/auth-check ────────────────────────────────── */

static esp_err_t auth_check_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    /* Check Bearer token first */
    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        const kvm_config_t *cfg = config_get();
        if (cfg->auth_token[0] != '\0') {
            char expected[96];
            snprintf(expected, sizeof(expected), "Bearer %s", cfg->auth_token);
            if (strcmp(auth_header, expected) == 0) {
                httpd_resp_sendstr(req, "{\"authorized\":true}");
                return ESP_OK;
            }
        }
    }

    httpd_resp_sendstr(req, "{\"authorized\":false,\"message\":\"Double-click the device button to authorize\"}");
    return ESP_OK;
}

/* ── Endpoint: GET /api/status ────────────────────────────────────── */

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "active_pc", cfg->active_pc);

    cJSON *pcs = cJSON_CreateArray();
    for (int i = 0; i < MAX_PC_COUNT; i++) {
        cJSON *pc = cJSON_CreateObject();
        cJSON_AddNumberToObject(pc, "id", i + 1);
        cJSON_AddStringToObject(pc, "name", cfg->pcs[i].name[0] ? cfg->pcs[i].name : "");
        bool connected;
        if (i == 2) {
#if HAS_USB
            connected = (cfg->usb_mode == USB_MODE_DEVICE) ? usb_device_is_connected() : false;
#else
            connected = false;
#endif
        } else {
            connected = ble_peripheral_is_pc_connected(i);  /* 0-indexed */
        }
        cJSON_AddBoolToObject(pc, "connected", connected);
        cJSON_AddStringToObject(pc, "type", (i == 2) ? "usb" : "ble");
        cJSON_AddItemToArray(pcs, pc);
    }
    cJSON_AddItemToObject(root, "pcs", pcs);

    cJSON *devices = cJSON_CreateObject();
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_HOST) {
        cJSON_AddBoolToObject(devices, "keyboard", usb_host_is_keyboard_connected());
        cJSON_AddBoolToObject(devices, "mouse", usb_host_is_mouse_connected());
        cJSON_AddStringToObject(devices, "input_source", "usb");
    } else {
        cJSON_AddBoolToObject(devices, "keyboard", ble_central_is_keyboard_connected());
        cJSON_AddBoolToObject(devices, "mouse", ble_central_is_mouse_connected());
        cJSON_AddStringToObject(devices, "input_source", "ble");
    }
#else
    cJSON_AddBoolToObject(devices, "keyboard", ble_central_is_keyboard_connected());
    cJSON_AddBoolToObject(devices, "mouse", ble_central_is_mouse_connected());
#endif
    cJSON_AddItemToObject(root, "devices", devices);

    cJSON *wifi = cJSON_CreateObject();
    wifi_operating_mode_t mode = wifi_manager_get_mode();
    const char *mode_str;
    switch (mode) {
    case KVM_WIFI_AP_ONLY:   mode_str = "ap"; break;
    case KVM_WIFI_STA_ONLY:  mode_str = "sta"; break;
    case KVM_WIFI_APSTA:     mode_str = "apsta"; break;
    case KVM_WIFI_OFF:       mode_str = "off"; break;
    default:                  mode_str = "unknown"; break;
    }
    cJSON_AddStringToObject(wifi, "mode", mode_str);
    cJSON_AddBoolToObject(wifi, "ap_active", wifi_manager_is_ap_active());
    cJSON_AddBoolToObject(wifi, "sta_connected", wifi_manager_is_sta_connected());
    cJSON_AddStringToObject(wifi, "sta_ip", wifi_manager_get_sta_ip());
    cJSON_AddStringToObject(wifi, "ap_ip", wifi_manager_get_ap_ip());
    cJSON_AddStringToObject(wifi, "ap_ssid", wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(wifi, "sta_ssid", cfg->wifi_ssid);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddNumberToObject(root, "input_mode", cfg->input_mode);
    cJSON_AddNumberToObject(root, "air_mouse_sensitivity", cfg->air_mouse_sensitivity);
    cJSON_AddNumberToObject(root, "screen_off_timeout_sec", cfg->screen_off_timeout_sec);
    cJSON_AddNumberToObject(root, "sleep_timeout_sec", cfg->sleep_timeout_sec);
#if HAS_BATTERY
    cJSON_AddStringToObject(root, "sleep_state",
        pm_sleep_get_state() == PM_STATE_ACTIVE ? "active" :
        pm_sleep_get_state() == PM_STATE_SCREEN_OFF ? "screen_off" : "sleep");
#endif

    cJSON_AddNumberToObject(root, "usb_mode", cfg->usb_mode);
    cJSON *usb = cJSON_CreateObject();
#if HAS_USB
    if (cfg->usb_mode == USB_MODE_DEVICE) {
        cJSON_AddStringToObject(usb, "mode", "device");
        cJSON_AddBoolToObject(usb, "connected", usb_device_is_connected());
    } else if (cfg->usb_mode == USB_MODE_HOST) {
        cJSON_AddStringToObject(usb, "mode", "host");
        cJSON_AddBoolToObject(usb, "keyboard_connected", usb_host_is_keyboard_connected());
        cJSON_AddBoolToObject(usb, "mouse_connected", usb_host_is_mouse_connected());
    } else {
        cJSON_AddStringToObject(usb, "mode", "disabled");
    }
#else
    cJSON_AddStringToObject(usb, "mode", "disabled");
#endif
    cJSON_AddItemToObject(root, "usb", usb);
    cJSON_AddBoolToObject(root, "voice_recording", voice_input_is_active());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: POST /api/switch ───────────────────────────────────── */

static esp_err_t switch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

#if HAS_BATTERY
    pm_sleep_on_activity();
#endif

    switch_manager_request_switch();

    /* Give the switch task time to process the command */
    vTaskDelay(pdMS_TO_TICKS(150));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "active_pc", switch_manager_get_active_pc());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: GET /api/logs (SSE, live only) ─────────────────────── */

#define MAX_LOG_CLIENTS 2
typedef struct {
    httpd_handle_t hd;
    int fd;
    bool active;
} log_client_t;

static log_client_t log_clients[MAX_LOG_CLIENTS];
static SemaphoreHandle_t log_mutex = NULL;

static void log_sse_broadcast(const char *event, const char *data)
{
    if (!log_mutex) return;
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_LOG_CLIENTS; i++) {
        if (!log_clients[i].active) continue;
        /* Build SSE packet with multi-line data support:
         * data may contain \r (from web_log replacing \n) —
         * split each \r into separate data: lines */
        char pkt[320];
        int pos = snprintf(pkt, sizeof(pkt), "event: %s\n", event);
        if (pos < 0 || pos >= (int)sizeof(pkt)) continue;
        const char *p = data;
        while (*p) {
            const char *next = strchr(p, '\r');
            if (next) {
                int seg_len = (int)(next - p);
                int remaining = (int)sizeof(pkt) - pos;
                if (remaining <= 0) break;
                pos += snprintf(pkt + pos, remaining, "data: %.*s\n", seg_len, p);
                p = next + 1;
            } else {
                int remaining = (int)sizeof(pkt) - pos;
                if (remaining > 0) {
                    pos += snprintf(pkt + pos, remaining, "data: %s\n", p);
                }
                break;
            }
            if (pos < 0 || pos >= (int)sizeof(pkt)) break;
        }
        if (pos >= 0 && pos < (int)sizeof(pkt)) {
            snprintf(pkt + pos, sizeof(pkt) - pos, "\n");
        }
        int sent = httpd_socket_send(log_clients[i].hd, log_clients[i].fd, pkt, pos, 0);
        if (sent < 0) {
            log_clients[i].active = false;
        }
    }
    xSemaphoreGive(log_mutex);
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!web_log_is_enabled()) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Web log is disabled");
        return ESP_FAIL;
    }

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_LOG_CLIENTS; i++) {
        if (!log_clients[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(log_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Too many log clients");
        return ESP_FAIL;
    }
    log_clients[slot].hd = req->handle;
    log_clients[slot].fd = httpd_req_to_sockfd(req);
    log_clients[slot].active = true;
    xSemaphoreGive(log_mutex);

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_sendstr(req, "event: connected\ndata: {}\n\n");

    /* Block until client disconnects.
     * recv(MSG_PEEK|MSG_DONTWAIT) returns:
     *   > 0  → data available, client still connected
     *   = 0  → EOF, client disconnected cleanly
     *   < 0  → error; EAGAIN/EWOULDBLOCK means no data yet (client still connected) */
    char buf[1];
    for (;;) {
        int ret = recv(httpd_req_to_sockfd(req), buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0) break;  /* Client disconnected */
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_clients[slot].active = false;
    xSemaphoreGive(log_mutex);
    return ESP_OK;
}

/* ── Endpoint: GET /api/events ────────────────────────────────────── */

static esp_err_t events_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (sse_add_client(req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Too many SSE clients");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_sendstr(req, "event: connected\ndata: {}\n\n");

    /* Block this task to keep the SSE connection alive.
     * recv(MSG_PEEK|MSG_DONTWAIT) returns:
     *   > 0  → data available, client still connected
     *   = 0  → EOF, client disconnected cleanly
     *   < 0  → error; EAGAIN/EWOULDBLOCK means no data yet (client still connected) */
    char buf[1];
    for (;;) {
        int ret = recv(httpd_req_to_sockfd(req), buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0) break;
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    sse_remove_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* ── Endpoint: POST /api/scan ─────────────────────────────────────── */

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    ble_central_start_scan();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: GET /api/scan ─────────────────────────────────────── */

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const char *results = ble_central_get_scan_results_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, results);
    return ESP_OK;
}

/* ── Endpoint: POST /api/pairings ──────────────────────────────── */

static esp_err_t pairings_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

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
            /* Save MAC to config so it auto-reconnects on reboot */
            kvm_config_t *cfg = config_get_mutable();
            memcpy(cfg->keyboard.mac, addr, 6);
            cfg->keyboard.addr_type = (uint8_t)atype_item->valueint;
            config_save_input_devices();
        } else if (strcmp(role_item->valuestring, "mouse") == 0) {
            ble_central_connect_mouse(addr, (uint8_t)atype_item->valueint);
            /* Save MAC to config so it auto-reconnects on reboot */
            kvm_config_t *cfg = config_get_mutable();
            memcpy(cfg->mouse.mac, addr, 6);
            cfg->mouse.addr_type = (uint8_t)atype_item->valueint;
            config_save_input_devices();
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

/* ── Endpoint: DELETE /api/pairings/{id} ───────────────────────── */

static esp_err_t pairings_delete_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const char *uri = req->uri;
    const char *prefix = "/api/pairings/";
    const char *id_start = uri + strlen(prefix);
    if (id_start <= uri || *id_start == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    int pair_id = atoi(id_start);
    kvm_config_t *cfg = config_get_mutable();
    bool deleted = false;

    if (pair_id >= 1 && pair_id <= MAX_PC_COUNT) {
        int idx = pair_id - 1;
        if (cfg->pcs[idx].pc_id == (uint8_t)pair_id) {
            /* Clear the PC pairing: clear MAC, but keep pc_id for the slot */
            memset(cfg->pcs[idx].identity_addr, 0, 6);
            cfg->pcs[idx].addr_type = 0;
            cfg->pcs[idx].name[0] = '\0';
            cfg->pcs[idx].conn_handle = 0;
            cfg->pcs[idx].connected = false;
            config_save_pcs();
            deleted = true;
            ESP_LOGI(TAG, "Deleted pairing for PC%d", pair_id);
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", deleted);
    cJSON_AddNumberToObject(root, "id", pair_id);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: GET /api/devices ───────────────────────────────────── */

static esp_err_t devices_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();

    cJSON *kb = cJSON_CreateObject();
    char kb_addr[18] = {0};
    snprintf(kb_addr, sizeof(kb_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             cfg->keyboard.mac[5], cfg->keyboard.mac[4], cfg->keyboard.mac[3],
             cfg->keyboard.mac[2], cfg->keyboard.mac[1], cfg->keyboard.mac[0]);
    cJSON_AddStringToObject(kb, "address", kb_addr);
    cJSON_AddStringToObject(kb, "name", cfg->keyboard.name[0] ? cfg->keyboard.name : "");
    cJSON_AddBoolToObject(kb, "connected", ble_central_is_keyboard_connected());
    cJSON_AddItemToObject(root, "keyboard", kb);

    cJSON *ms = cJSON_CreateObject();
    char ms_addr[18] = {0};
    snprintf(ms_addr, sizeof(ms_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             cfg->mouse.mac[5], cfg->mouse.mac[4], cfg->mouse.mac[3],
             cfg->mouse.mac[2], cfg->mouse.mac[1], cfg->mouse.mac[0]);
    cJSON_AddStringToObject(ms, "address", ms_addr);
    cJSON_AddStringToObject(ms, "name", cfg->mouse.name[0] ? cfg->mouse.name : "");
    cJSON_AddBoolToObject(ms, "connected", ble_central_is_mouse_connected());
    cJSON_AddItemToObject(root, "mouse", ms);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: GET /api/wifi ─────────────────────────────────────── */

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

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

/* ── Endpoint: PATCH /api/wifi ──────────────────────────────────── */

static esp_err_t wifi_patch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

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

/* ── Endpoint: GET /api/settings ──────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();

    cJSON *pc_names = cJSON_CreateObject();
    for (int i = 0; i < MAX_PC_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "pc%d", i + 1);
        cJSON_AddStringToObject(pc_names, key, cfg->pcs[i].name[0] ? cfg->pcs[i].name : "");
    }
    cJSON_AddItemToObject(root, "pc_names", pc_names);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name[0] ? cfg->device_name : "");
    cJSON_AddBoolToObject(root, "wifi_enabled", cfg->wifi_enabled);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddBoolToObject(root, "anti_idle", cfg->anti_idle_enabled);
    cJSON_AddNumberToObject(root, "anti_idle_interval", cfg->anti_idle_interval_sec);
    cJSON_AddNumberToObject(root, "input_mode", cfg->input_mode);
    cJSON_AddNumberToObject(root, "air_mouse_sensitivity", cfg->air_mouse_sensitivity);
    cJSON_AddNumberToObject(root, "usb_mode", cfg->usb_mode);
    cJSON_AddStringToObject(root, "keyboard_name", cfg->keyboard.name[0] ? cfg->keyboard.name : "");
    cJSON_AddStringToObject(root, "mouse_name", cfg->mouse.name[0] ? cfg->mouse.name : "");
    cJSON_AddBoolToObject(root, "web_log_enabled", web_log_is_enabled());
    cJSON_AddBoolToObject(root, "voice_asr_enabled", cfg->voice_asr_enabled);
    cJSON_AddNumberToObject(root, "voice_asr_appid", cfg->voice_asr_appid);
    if (cfg->voice_asr_api_key[0]) {
        int klen = strlen(cfg->voice_asr_api_key);
        int show = klen > 4 ? 4 : klen;
        char masked[65] = {0};
        memset(masked, '*', klen - show);
        memcpy(masked + klen - show, cfg->voice_asr_api_key + klen - show, show);
        cJSON_AddStringToObject(root, "voice_asr_api_key", masked);
    } else {
        cJSON_AddStringToObject(root, "voice_asr_api_key", "");
    }
    cJSON_AddStringToObject(root, "voice_lang", cfg->voice_lang);
    cJSON_AddNumberToObject(root, "voice_input_mode", cfg->voice_input_mode);
    cJSON_AddNumberToObject(root, "screen_off_timeout_sec", cfg->screen_off_timeout_sec);
    cJSON_AddNumberToObject(root, "sleep_timeout_sec", cfg->sleep_timeout_sec);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: PATCH /api/settings ─────────────────────────────────── */

static esp_err_t settings_patch_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

#if HAS_BATTERY
    pm_sleep_on_activity();
#endif

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

    cJSON *dev_name = cJSON_GetObjectItem(body, "device_name");
    if (cJSON_IsString(dev_name)) {
        strncpy(cfg->device_name, dev_name->valuestring, DEVICE_NAME_MAX - 1);
        cfg->device_name[DEVICE_NAME_MAX - 1] = '\0';
        config_save_device_name();
        /* Update BLE advertising name */
        ble_svc_gap_device_name_set(cfg->device_name);
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

    cJSON *im = cJSON_GetObjectItem(body, "input_mode");
    if (cJSON_IsNumber(im)) {
        int val = im->valueint;
        if (val >= 0 && val <= 1) {
            input_mode_set((input_mode_t)val);
        }
    }

    cJSON *sens = cJSON_GetObjectItem(body, "air_mouse_sensitivity");
    if (cJSON_IsNumber(sens)) {
        int val = sens->valueint;
        if (val >= 1 && val <= 10) {
            config_get_mutable()->air_mouse_sensitivity = (uint8_t)val;
            config_save_input_mode();
        }
    }

    cJSON *usb_mode_item = cJSON_GetObjectItem(body, "usb_mode");
    if (cJSON_IsNumber(usb_mode_item)) {
        int val = usb_mode_item->valueint;
        if (val >= USB_MODE_DISABLED && val <= USB_MODE_HOST) {
            uint8_t old_mode = cfg->usb_mode;
            cfg->usb_mode = (uint8_t)val;
            config_save_usb_mode();
            if (val != old_mode) {
                cJSON_Delete(body);
                cJSON *root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "ok", true);
                cJSON_AddStringToObject(root, "message", "Reboot required for USB mode change");
                cJSON_AddBoolToObject(root, "reboot_required", true);
                char *json = cJSON_PrintUnformatted(root);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, json);
                cJSON_free(json);
                cJSON_Delete(root);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
                return ESP_OK;
            }
        }
    }

    cJSON *kb_name = cJSON_GetObjectItem(body, "keyboard_name");
    if (cJSON_IsString(kb_name)) {
        strncpy(cfg->keyboard.name, kb_name->valuestring, DEVICE_NAME_MAX - 1);
        cfg->keyboard.name[DEVICE_NAME_MAX - 1] = '\0';
        config_save_input_devices();
    }

    cJSON *ms_name = cJSON_GetObjectItem(body, "mouse_name");
    if (cJSON_IsString(ms_name)) {
        strncpy(cfg->mouse.name, ms_name->valuestring, DEVICE_NAME_MAX - 1);
        cfg->mouse.name[DEVICE_NAME_MAX - 1] = '\0';
        config_save_input_devices();
    }

    cJSON *web_log_item = cJSON_GetObjectItem(body, "web_log_enabled");
    if (cJSON_IsBool(web_log_item)) {
        if (cJSON_IsTrue(web_log_item)) {
            web_log_enable();
        } else {
            web_log_disable();
        }
    }

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

    if (voice_en_item || voice_appid_item || voice_ak_item || voice_lang_item || voice_im_item) {
        config_save_voice();
    }

    cJSON *scr_off = cJSON_GetObjectItem(body, "screen_off_timeout_sec");
    if (cJSON_IsNumber(scr_off)) {
        cfg->screen_off_timeout_sec = (uint16_t)scr_off->valueint;
    }

    cJSON *sleep_to = cJSON_GetObjectItem(body, "sleep_timeout_sec");
    if (cJSON_IsNumber(sleep_to)) {
        cfg->sleep_timeout_sec = (uint16_t)sleep_to->valueint;
    }

    if (scr_off || sleep_to) {
        config_save_sleep();
    }

    cJSON *factory_reset = cJSON_GetObjectItem(body, "factory_reset");
    if (cJSON_IsTrue(factory_reset)) {
        cJSON_Delete(body);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "Factory reset initiated");
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

/* ── Endpoint: POST /api/pairing/start ───────────────────────────── */

static esp_err_t pairing_start_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    ble_peripheral_enter_pairing_mode();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "Pairing mode active for 60 seconds");
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: POST /api/pairing/stop ────────────────────────────── */

static esp_err_t pairing_stop_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    ble_peripheral_exit_pairing_mode();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Notification functions ───────────────────────────────────────── */

void web_server_notify_switch(uint8_t active_pc)
{
    char data[32];
    snprintf(data, sizeof(data), "{\"active_pc\":%d}", active_pc);
    sse_broadcast("switch", data);
}

void web_server_notify_connection(uint8_t pc_id, bool connected)
{
    char data[64];
    snprintf(data, sizeof(data), "{\"pc_id\":%d,\"connected\":%s}",
             pc_id, connected ? "true" : "false");
    sse_broadcast("connection", data);
}

void web_server_notify_device(const char *device_type, bool connected)
{
    char data[64];
    snprintf(data, sizeof(data), "{\"device\":\"%s\",\"connected\":%s}",
             device_type, connected ? "true" : "false");
    sse_broadcast("device", data);
}

/* ── URI registration table ───────────────────────────────────────── */

static const httpd_uri_t uris[] = {
    { .uri = "/",               .method = HTTP_GET,    .handler = root_handler },
    { .uri = "/api/auth-check", .method = HTTP_GET,    .handler = auth_check_handler },
    { .uri = "/api/status",     .method = HTTP_GET,    .handler = status_handler },
    { .uri = "/api/switch",     .method = HTTP_POST,   .handler = switch_handler },
    { .uri = "/api/events",     .method = HTTP_GET,    .handler = events_handler },
    { .uri = "/api/pairings",   .method = HTTP_POST,   .handler = pairings_post_handler },
    { .uri = "/api/pairings/*", .method = HTTP_DELETE, .handler = pairings_delete_handler },
    { .uri = "/api/pair/pc",    .method = HTTP_POST,   .handler = pairings_post_handler },
    { .uri = "/api/pair/keyboard", .method = HTTP_POST, .handler = pairings_post_handler },
    { .uri = "/api/pair/mouse", .method = HTTP_POST,   .handler = pairings_post_handler },
    { .uri = "/api/scan",       .method = HTTP_POST,   .handler = scan_handler },
    { .uri = "/api/scan",       .method = HTTP_GET,    .handler = scan_get_handler },
    { .uri = "/api/scan/results", .method = HTTP_GET,  .handler = scan_get_handler },
    { .uri = "/api/devices",    .method = HTTP_GET,    .handler = devices_handler },
    { .uri = "/api/wifi",       .method = HTTP_GET,    .handler = wifi_get_handler },
    { .uri = "/api/wifi",       .method = HTTP_PATCH,  .handler = wifi_patch_handler },
    { .uri = "/api/wifi",       .method = HTTP_POST,   .handler = wifi_patch_handler },
    { .uri = "/api/settings",   .method = HTTP_GET,    .handler = settings_get_handler },
    { .uri = "/api/settings",   .method = HTTP_PATCH,  .handler = settings_patch_handler },
    { .uri = "/api/settings",   .method = HTTP_POST,   .handler = settings_patch_handler },
    { .uri = "/api/logs",       .method = HTTP_GET,    .handler = logs_handler },
    { .uri = "/api/pairing/start", .method = HTTP_POST, .handler = pairing_start_handler },
    { .uri = "/api/pairing/stop",  .method = HTTP_POST, .handler = pairing_stop_handler },
};

#define NUM_URIS (sizeof(uris) / sizeof(uris[0]))

/* ── Init ─────────────────────────────────────────────────────────── */

void web_server_init(void)
{
    sse_mutex = xSemaphoreCreateMutex();
    memset(sse_clients, 0, sizeof(sse_clients));

    log_mutex = xSemaphoreCreateMutex();
    memset(log_clients, 0, sizeof(log_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = NUM_URIS + 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Web server start failed, retrying after delay...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (httpd_start(&server, &config) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start web server");
            return;
        }
    }

    for (int i = 0; i < (int)NUM_URIS; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    /* Start keepalive timer */
    esp_timer_handle_t keepalive_timer;
    esp_timer_create_args_t timer_args = {
        .callback = sse_keepalive_cb,
        .name = "sse_keepalive",
    };
    esp_timer_create(&timer_args, &keepalive_timer);
    esp_timer_start_periodic(keepalive_timer, SSE_KEEPALIVE_INTERVAL_MS * 1000);

    /* Register log SSE broadcast so web_log can push to log clients */
    web_log_register_sse_broadcast(log_sse_broadcast);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}
