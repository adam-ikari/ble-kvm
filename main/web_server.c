#include "web_server.h"
#include "config_manager.h"
#include "switch_manager.h"
#include "ble_central.h"
#include "ble_peripheral.h"
#include "wifi_manager.h"
#include "web_dist_gz.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

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

static bool check_auth(httpd_req_t *req)
{
    char auth_header[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        const char *token = config_get()->auth_token;
        char expected[64];
        snprintf(expected, sizeof(expected), "Bearer %s", token);
        if (strcmp(auth_header, expected) == 0) return true;
    }
    /* Check query param for SSE */
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char token_param[32] = {0};
        if (httpd_query_key_value(query, "token", token_param, sizeof(token_param)) == ESP_OK) {
            if (strcmp(token_param, config_get()->auth_token) == 0) return true;
        }
    }
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return false;
}

/* ── SSE helpers ──────────────────────────────────────────────────── */

static void sse_send_to_client(sse_client_t *client, const char *event, const char *data)
{
    char pkt[256];
    int len = snprintf(pkt, sizeof(pkt), "event: %s\ndata: %s\n\n", event, data);
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)web_dist_index_html_gz, web_dist_index_html_gz_len);
    return ESP_OK;
}

/* ── Endpoint: GET /api/status ────────────────────────────────────── */

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "active_pc", cfg->active_pc);

    cJSON *pcs = cJSON_CreateArray();
    for (int i = 0; i < MAX_PC_COUNT; i++) {
        cJSON *pc = cJSON_CreateObject();
        cJSON_AddNumberToObject(pc, "id", i + 1);
        cJSON_AddStringToObject(pc, "name", cfg->pcs[i].name[0] ? cfg->pcs[i].name : "");
        cJSON_AddBoolToObject(pc, "connected", ble_peripheral_is_pc_connected(i + 1));
        cJSON_AddItemToArray(pcs, pc);
    }
    cJSON_AddItemToObject(root, "pcs", pcs);

    cJSON *devices = cJSON_CreateObject();
    cJSON_AddBoolToObject(devices, "keyboard", ble_central_is_keyboard_connected());
    cJSON_AddBoolToObject(devices, "mouse", ble_central_is_mouse_connected());
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
    if (!check_auth(req)) return ESP_FAIL;

    switch_manager_request_switch(SWITCH_SRC_WEB);

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

/* ── Endpoint: GET /api/events ────────────────────────────────────── */

static esp_err_t events_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    if (sse_add_client(req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Too many SSE clients");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_sendstr(req, "event: connected\ndata: {}\n\n");

    /* Block this task to keep the SSE connection alive.
     * When the client disconnects, httpd will close the socket,
     * and we detect this by trying a recv. */
    char buf[1];
    while (recv(httpd_req_to_sockfd(req), buf, 1, MSG_PEEK | MSG_DONTWAIT) >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    sse_remove_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* ── Endpoint: POST /api/scan ─────────────────────────────────────── */

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

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

/* ── Endpoint: GET /api/scan/results ──────────────────────────────── */

static esp_err_t scan_results_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    const char *results = ble_central_get_scan_results_json();
    if (!results) {
        results = "[]";
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, results);
    return ESP_OK;
}

/* ── Endpoint: POST /api/pair/keyboard ────────────────────────────── */

static esp_err_t pair_keyboard_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[128] = {0};
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

    cJSON *addr_item = cJSON_GetObjectItem(body, "address");
    cJSON *type_item = cJSON_GetObjectItem(body, "addr_type");
    if (!cJSON_IsString(addr_item) || !cJSON_IsNumber(type_item)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing address or addr_type");
        return ESP_FAIL;
    }

    uint8_t addr[6] = {0};
    const char *addr_str = addr_item->valuestring;
    unsigned int tmp[6];
    if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &tmp[5], &tmp[4], &tmp[3], &tmp[2], &tmp[1], &tmp[0]) == 6) {
        for (int i = 0; i < 6; i++) addr[i] = (uint8_t)tmp[i];
    }

    ble_central_connect_keyboard(addr, (uint8_t)type_item->valueint);
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

/* ── Endpoint: POST /api/pair/mouse ───────────────────────────────── */

static esp_err_t pair_mouse_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    char buf[128] = {0};
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

    cJSON *addr_item = cJSON_GetObjectItem(body, "address");
    cJSON *type_item = cJSON_GetObjectItem(body, "addr_type");
    if (!cJSON_IsString(addr_item) || !cJSON_IsNumber(type_item)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing address or addr_type");
        return ESP_FAIL;
    }

    uint8_t addr[6] = {0};
    const char *addr_str = addr_item->valuestring;
    unsigned int tmp[6];
    if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &tmp[5], &tmp[4], &tmp[3], &tmp[2], &tmp[1], &tmp[0]) == 6) {
        for (int i = 0; i < 6; i++) addr[i] = (uint8_t)tmp[i];
    }

    ble_central_connect_mouse(addr, (uint8_t)type_item->valueint);
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

/* ── Endpoint: POST /api/pair/pc ──────────────────────────────────── */

static esp_err_t pair_pc_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_FAIL;

    ble_peripheral_start_advertising();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
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
    if (!check_auth(req)) return ESP_FAIL;

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

/* ── Endpoint: POST /api/wifi ─────────────────────────────────────── */

static esp_err_t wifi_handler(httpd_req_t *req)
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

    // Handle mode change
    cJSON *mode_item = cJSON_GetObjectItem(body, "mode");
    if (cJSON_IsString(mode_item)) {
        const char *mode_str = mode_item->valuestring;
        if (strcmp(mode_str, "ap") == 0) {
            wifi_manager_set_mode(KVM_WIFI_AP_ONLY);
        } else if (strcmp(mode_str, "sta") == 0) {
            wifi_manager_set_mode(KVM_WIFI_STA_ONLY);
        } else if (strcmp(mode_str, "apsta") == 0) {
            wifi_manager_set_mode(KVM_WIFI_APSTA);
        } else if (strcmp(mode_str, "off") == 0) {
            wifi_manager_set_mode(KVM_WIFI_OFF);
        }
    }

    // Handle STA connect
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

    // Handle STA disconnect
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
    if (!check_auth(req)) return ESP_FAIL;

    const kvm_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();

    cJSON *pc_names = cJSON_CreateObject();
    for (int i = 0; i < MAX_PC_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "pc%d", i + 1);
        cJSON_AddStringToObject(pc_names, key, cfg->pcs[i].name[0] ? cfg->pcs[i].name : "");
    }
    cJSON_AddItemToObject(root, "pc_names", pc_names);
    cJSON_AddBoolToObject(root, "wifi_enabled", cfg->wifi_enabled);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Endpoint: POST /api/settings ─────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
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

    /* PC name updates */
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

    /* Token regeneration */
    cJSON *regen = cJSON_GetObjectItem(body, "regenerate_token");
    if (cJSON_IsTrue(regen)) {
        config_generate_auth_token();
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
    { .uri = "/",               .method = HTTP_GET,  .handler = root_handler },
    { .uri = "/api/status",     .method = HTTP_GET,  .handler = status_handler },
    { .uri = "/api/switch",     .method = HTTP_POST, .handler = switch_handler },
    { .uri = "/api/events",     .method = HTTP_GET,  .handler = events_handler },
    { .uri = "/api/scan",       .method = HTTP_POST, .handler = scan_handler },
    { .uri = "/api/scan/results", .method = HTTP_GET, .handler = scan_results_handler },
    { .uri = "/api/pair/keyboard", .method = HTTP_POST, .handler = pair_keyboard_handler },
    { .uri = "/api/pair/mouse",    .method = HTTP_POST, .handler = pair_mouse_handler },
    { .uri = "/api/pair/pc",       .method = HTTP_POST, .handler = pair_pc_handler },
    { .uri = "/api/devices",    .method = HTTP_GET,  .handler = devices_handler },
    { .uri = "/api/wifi",       .method = HTTP_POST, .handler = wifi_handler },
    { .uri = "/api/settings",   .method = HTTP_GET,  .handler = settings_get_handler },
    { .uri = "/api/settings",   .method = HTTP_POST, .handler = settings_post_handler },
};

#define NUM_URIS (sizeof(uris) / sizeof(uris[0]))

/* ── Init ─────────────────────────────────────────────────────────── */

void web_server_init(void)
{
    sse_mutex = xSemaphoreCreateMutex();
    memset(sse_clients, 0, sizeof(sse_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = NUM_URIS + 2;  /* a little headroom */
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
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

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}
