#include "ble_central.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "esp_log.h"
#include "event_bus.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "ble_central";

/* Maximum scan results */
#define MAX_SCAN_RESULTS 20

/* HID Service UUID */
#define BLE_SVC_HID_UUID16 0x1812

/* HID Report characteristic UUID */
#define BLE_SVC_HID_CHR_REPORT 0x2A4D

/* CCCD UUID */
#define BLE_GATT_DSC_CLT_CFG_UUID16 0x2902

/* Scan result entry */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[32];
    bool has_keyboard;
    bool has_mouse;
} scan_result_t;

static scan_result_t scan_results[MAX_SCAN_RESULTS];
static uint8_t scan_result_count = 0;
static bool scanning = false;

/* Connection state */
static uint16_t keyboard_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t mouse_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/* GATT discovery state per connection */
typedef struct {
    uint16_t conn_handle;
    uint16_t hid_svc_start_handle;
    uint16_t hid_svc_end_handle;
    bool svc_found;
    bool subscribing;
    uint8_t next_chr_idx;
} gatt_disc_state_t;

static gatt_disc_state_t keyboard_disc;
static gatt_disc_state_t mouse_disc;

/* Report characteristic handles (per device) */
#define MAX_REPORT_CHRS 4

typedef struct {
    uint16_t chr_val_handle;
    uint16_t cccd_handle;
    bool subscribed;
} report_chr_info_t;

typedef struct {
    uint16_t conn_handle;
    report_chr_info_t reports[MAX_REPORT_CHRS];
    uint8_t report_count;
    bool is_keyboard;
} device_gatt_info_t;

static device_gatt_info_t keyboard_gatt;
static device_gatt_info_t mouse_gatt;

/* Forward declarations */
static int ble_central_gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void attempt_reconnect(const input_device_t *dev, bool is_keyboard);
static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg);
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);
static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg);
static int on_subscribe_write(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg);
static void subscribe_to_reports(uint16_t conn_handle, device_gatt_info_t *gatt);

/* ----- Scan Results JSON ----- */

const char *ble_central_get_scan_results_json(void)
{
    static char json_buf[2048];
    int offset = 0;

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                       "{\"results\":[");

    for (int i = 0; i < scan_result_count; i++) {
        if (i > 0) {
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, ",");
        }
        const scan_result_t *r = &scan_results[i];
        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                           "{\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                           "\"addr_type\":%d,"
                           "\"name\":\"%s\","
                           "\"has_keyboard\":%s,"
                           "\"has_mouse\":%s}",
                           r->addr[5], r->addr[4], r->addr[3],
                           r->addr[2], r->addr[1], r->addr[0],
                           r->addr_type,
                           r->name,
                           r->has_keyboard ? "true" : "false",
                           r->has_mouse ? "true" : "false");
    }

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");
    return json_buf;
}

/* ----- Scan Helpers ----- */

static bool addr_in_scan_results(const uint8_t *addr, uint8_t addr_type)
{
    for (int i = 0; i < scan_result_count; i++) {
        if (scan_results[i].addr_type == addr_type &&
            memcmp(scan_results[i].addr, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void add_scan_result(const uint8_t *addr, uint8_t addr_type,
                            const char *name, bool has_keyboard, bool has_mouse)
{
    if (scan_result_count >= MAX_SCAN_RESULTS) {
        return;
    }
    if (addr_in_scan_results(addr, addr_type)) {
        /* Update existing entry with new capabilities */
        for (int i = 0; i < scan_result_count; i++) {
            if (scan_results[i].addr_type == addr_type &&
                memcmp(scan_results[i].addr, addr, 6) == 0) {
                if (has_keyboard) scan_results[i].has_keyboard = true;
                if (has_mouse) scan_results[i].has_mouse = true;
                break;
            }
        }
        return;
    }

    scan_result_t *r = &scan_results[scan_result_count++];
    memcpy(r->addr, addr, 6);
    r->addr_type = addr_type;
    if (name) {
        strncpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    } else {
        r->name[0] = '\0';
    }
    r->has_keyboard = has_keyboard;
    r->has_mouse = has_mouse;

    ESP_LOGI(TAG, "Scan result: %02x:%02x:%02x:%02x:%02x:%02x type=%d name=%s kb=%d ms=%d",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             addr_type, r->name, has_keyboard, has_mouse);
}

/* ----- GAP Event Handler ----- */

static int ble_central_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        /* Parse advertising data for HID Service UUID */
        {
            struct ble_hs_adv_fields fields;
            rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                         event->disc.length_data);
            if (rc != 0) {
                break;
            }

            bool has_hid = false;

            /* Check UUID16 list for HID Service */
            if (fields.uuids16 != NULL) {
                for (int i = 0; i < fields.num_uuids16; i++) {
                    if (ble_uuid_u16(&fields.uuids16[i].u) == BLE_SVC_HID_UUID16) {
                        has_hid = true;
                        break;
                    }
                }
            }

            /* Check complete UUID16 list */
            if (!has_hid && fields.uuids16_is_complete && fields.uuids16 != NULL) {
                for (int i = 0; i < fields.num_uuids16; i++) {
                    if (ble_uuid_u16(&fields.uuids16[i].u) == BLE_SVC_HID_UUID16) {
                        has_hid = true;
                        break;
                    }
                }
            }

            if (!has_hid) {
                break;
            }

            /* Extract device name */
            char name[32] = {0};
            if (fields.name != NULL && fields.name_len > 0) {
                int len = fields.name_len;
                if (len >= (int)sizeof(name)) {
                    len = sizeof(name) - 1;
                }
                memcpy(name, fields.name, len);
                name[len] = '\0';
            }

            /* For now, mark as both keyboard and mouse;
             * actual determination happens after GATT discovery */
            add_scan_result(event->disc.addr.val, event->disc.addr.type,
                            name, true, true);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_CONNECT; status=%d", event->connect.status);

        if (event->connect.status == 0) {
            /* Connection successful */
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to find conn descriptor");
                break;
            }

            /* Determine if this is keyboard or mouse based on which
             * initiated the connection */
            gatt_disc_state_t *disc = NULL;
            device_gatt_info_t *gatt = NULL;
            bool is_keyboard;

            if (keyboard_conn_handle == 0xFFFF) {
                /* Pending keyboard connection confirmed */
                keyboard_conn_handle = event->connect.conn_handle;
                is_keyboard = true;
                disc = &keyboard_disc;
                gatt = &keyboard_gatt;
            } else if (mouse_conn_handle == 0xFFFF) {
                /* Pending mouse connection confirmed */
                mouse_conn_handle = event->connect.conn_handle;
                is_keyboard = false;
                disc = &mouse_disc;
                gatt = &mouse_gatt;
            } else {
                ESP_LOGW(TAG, "Unknown connection handle");
                break;
            }

            memset(disc, 0, sizeof(*disc));
            disc->conn_handle = event->connect.conn_handle;

            memset(gatt, 0, sizeof(*gatt));
            gatt->conn_handle = event->connect.conn_handle;
            gatt->is_keyboard = is_keyboard;

            ESP_LOGI(TAG, "%s connected (handle=%d)",
                     is_keyboard ? "Keyboard" : "Mouse",
                     event->connect.conn_handle);

            /* Post connection event */
            app_evt_device_connected_t evt = {
                .conn_handle = event->connect.conn_handle
            };
            APP_EVENT_POST(is_keyboard ? APP_EVENT_KB_CONNECTED : APP_EVENT_MS_CONNECTED,
                           &evt, sizeof(evt));

            /* Start GATT service discovery for HID Service */
            ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BLE_SVC_HID_UUID16);
            rc = ble_gattc_disc_svc_by_uuid(event->connect.conn_handle,
                                            &hid_uuid.u,
                                            on_svc_disc, disc);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover HID service: rc=%d", rc);
            }
        } else {
            ESP_LOGE(TAG, "Connection failed: status=%d", event->connect.status);

            /* Clear pending sentinel on failure */
            if (keyboard_conn_handle == 0xFFFF) {
                keyboard_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
            if (mouse_conn_handle == 0xFFFF) {
                mouse_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }

            /* Try reconnect from config */
            const kvm_config_t *cfg = config_get();
            bool nonzero = false;
            for (int i = 0; i < 6; i++) {
                if (cfg->keyboard.mac[i] != 0) { nonzero = true; break; }
            }
            if (nonzero && keyboard_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                attempt_reconnect(&cfg->keyboard, true);
            }

            nonzero = false;
            for (int i = 0; i < 6; i++) {
                if (cfg->mouse.mac[i] != 0) { nonzero = true; break; }
            }
            if (nonzero && mouse_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                attempt_reconnect(&cfg->mouse, false);
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_DISCONNECT; reason=%d", event->disconnect.reason);

        if (event->disconnect.conn.conn_handle == keyboard_conn_handle) {
            ESP_LOGI(TAG, "Keyboard disconnected");
            APP_EVENT_POST(APP_EVENT_KB_DISCONNECTED, NULL, 0);
            keyboard_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            memset(&keyboard_gatt, 0, sizeof(keyboard_gatt));
            memset(&keyboard_disc, 0, sizeof(keyboard_disc));

            /* Auto-reconnect from config */
            const kvm_config_t *cfg = config_get();
            bool nonzero = false;
            for (int i = 0; i < 6; i++) {
                if (cfg->keyboard.mac[i] != 0) { nonzero = true; break; }
            }
            if (nonzero) {
                attempt_reconnect(&cfg->keyboard, true);
            }
        } else if (event->disconnect.conn.conn_handle == mouse_conn_handle) {
            ESP_LOGI(TAG, "Mouse disconnected");
            APP_EVENT_POST(APP_EVENT_MS_DISCONNECTED, NULL, 0);
            mouse_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            memset(&mouse_gatt, 0, sizeof(mouse_gatt));
            memset(&mouse_disc, 0, sizeof(mouse_disc));

            /* Auto-reconnect from config */
            const kvm_config_t *cfg2 = config_get();
            bool nonzero2 = false;
            for (int i = 0; i < 6; i++) {
                if (cfg2->mouse.mac[i] != 0) { nonzero2 = true; break; }
            }
            if (nonzero2) {
                attempt_reconnect(&cfg2->mouse, false);
            }
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Post HID data events */
        {
            uint16_t ch = event->notify_rx.conn_handle;
            const uint8_t *data;
            uint16_t data_len;

            data = event->notify_rx.om->om_data;
            data_len = OS_MBUF_PKTLEN(event->notify_rx.om);

            app_evt_hid_data_t hid_evt;
            hid_evt.len = (data_len > HID_DATA_MAX_LEN) ? HID_DATA_MAX_LEN : (uint8_t)data_len;
            memcpy(hid_evt.data, data, hid_evt.len);
            if (ch == keyboard_conn_handle) {
                APP_EVENT_POST(APP_EVENT_HID_KEYBOARD_DATA, &hid_evt, sizeof(hid_evt));
            } else if (ch == mouse_conn_handle) {
                APP_EVENT_POST(APP_EVENT_HID_MOUSE_DATA, &hid_evt, sizeof(hid_evt));
            }
        }
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_DISC_COMPLETE");
        scanning = false;
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_MTU; conn_handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }

    return 0;
}

/* ----- GATT Discovery Callbacks ----- */

/* Service discovery callback */
static int on_svc_disc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc,
                       void *arg)
{
    gatt_disc_state_t *disc = (gatt_disc_state_t *)arg;

    if (error->status == 0) {
        /* Found HID Service */
        disc->hid_svc_start_handle = svc->start_handle;
        disc->hid_svc_end_handle = svc->end_handle;
        disc->svc_found = true;
        ESP_LOGI(TAG, "HID Service found: start=%d end=%d",
                 svc->start_handle, svc->end_handle);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        /* Service discovery complete */
        if (!disc->svc_found) {
            ESP_LOGW(TAG, "HID Service not found on conn_handle=%d", conn_handle);
            return 0;
        }

        /* Now discover characteristics within the HID Service */
        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                         disc->hid_svc_start_handle,
                                         disc->hid_svc_end_handle,
                                         on_chr_disc, disc);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to discover characteristics: rc=%d", rc);
        }
        return 0;
    }

    ESP_LOGE(TAG, "Service discovery error: status=%d", error->status);
    return 0;
}

/* Characteristic discovery callback */
static int on_chr_disc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr,
                       void *arg)
{
    gatt_disc_state_t *disc = (gatt_disc_state_t *)arg;
    device_gatt_info_t *gatt = NULL;

    /* Determine which gatt info to use */
    if (conn_handle == keyboard_conn_handle) {
        gatt = &keyboard_gatt;
    } else if (conn_handle == mouse_conn_handle) {
        gatt = &mouse_gatt;
    } else {
        return 0;
    }

    if (error->status == 0 && chr != NULL) {
        /* Check if this is a Report characteristic */
        if (ble_uuid_u16(&chr->uuid.u) == BLE_SVC_HID_CHR_REPORT) {
            if (gatt->report_count < MAX_REPORT_CHRS) {
                gatt->reports[gatt->report_count].chr_val_handle = chr->val_handle;
                gatt->reports[gatt->report_count].cccd_handle = 0;
                gatt->reports[gatt->report_count].subscribed = false;
                gatt->report_count++;
                ESP_LOGI(TAG, "Report chr found: val_handle=%d", chr->val_handle);
            }
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        /* Characteristic discovery complete, now discover descriptors
         * for each Report characteristic to find CCCDs */
        disc->subscribing = true;
        disc->next_chr_idx = 0;

        if (gatt->report_count > 0) {
            /* Discover descriptors for the first report characteristic */
            uint16_t start_handle = gatt->reports[0].chr_val_handle + 1;

            /* End handle is the start of the next characteristic or service end */
            uint16_t end_handle;
            if (gatt->report_count > 1) {
                end_handle = gatt->reports[1].chr_val_handle - 1;
            } else {
                end_handle = disc->hid_svc_end_handle;
            }

            if (start_handle > end_handle) {
                /* No descriptors, try subscribing directly */
                subscribe_to_reports(conn_handle, gatt);
            } else {
                int rc = ble_gattc_disc_all_dscs(conn_handle,
                                                 start_handle,
                                                 end_handle,
                                                 on_dsc_disc, disc);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Failed to discover descriptors: rc=%d", rc);
                    /* Try subscribing anyway */
                    subscribe_to_reports(conn_handle, gatt);
                }
            }
        } else {
            ESP_LOGW(TAG, "No Report characteristics found");
        }
        return 0;
    }

    ESP_LOGE(TAG, "Characteristic discovery error: status=%d", error->status);
    return 0;
}

/* Descriptor discovery callback */
static int on_dsc_disc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    gatt_disc_state_t *disc = (gatt_disc_state_t *)arg;
    device_gatt_info_t *gatt = NULL;

    if (conn_handle == keyboard_conn_handle) {
        gatt = &keyboard_gatt;
    } else if (conn_handle == mouse_conn_handle) {
        gatt = &mouse_gatt;
    } else {
        return 0;
    }

    if (error->status == 0 && dsc != NULL) {
        /* Check if this is a CCCD */
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            /* Find which report characteristic this descriptor belongs to */
            for (uint8_t i = 0; i < gatt->report_count; i++) {
                if (gatt->reports[i].chr_val_handle == chr_val_handle) {
                    gatt->reports[i].cccd_handle = dsc->handle;
                    ESP_LOGI(TAG, "CCCD found for report chr %d: handle=%d",
                             chr_val_handle, dsc->handle);
                    break;
                }
            }
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        /* Descriptor discovery for current characteristic complete,
         * move to next report characteristic */
        disc->next_chr_idx++;

        if (disc->next_chr_idx < gatt->report_count) {
            uint8_t idx = disc->next_chr_idx;
            uint16_t start_handle = gatt->reports[idx].chr_val_handle + 1;
            uint16_t end_handle;
            if (idx + 1 < gatt->report_count) {
                end_handle = gatt->reports[idx + 1].chr_val_handle - 1;
            } else {
                end_handle = disc->hid_svc_end_handle;
            }

            if (start_handle <= end_handle) {
                int rc = ble_gattc_disc_all_dscs(conn_handle,
                                                 start_handle,
                                                 end_handle,
                                                 on_dsc_disc, disc);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Failed to discover descriptors for chr %d: rc=%d", idx, rc);
                    /* Proceed with subscription */
                    subscribe_to_reports(conn_handle, gatt);
                }
            }
        } else {
            /* All descriptors discovered, now subscribe */
            subscribe_to_reports(conn_handle, gatt);
        }
        return 0;
    }

    ESP_LOGE(TAG, "Descriptor discovery error: status=%d", error->status);
    return 0;
}

/* ----- Subscribe to HID Report Notifications ----- */

static int on_subscribe_write(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr,
                              void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to notifications on conn_handle=%d", conn_handle);
    } else {
        ESP_LOGE(TAG, "Subscribe write failed: status=%d", error->status);
    }
    return 0;
}

static void subscribe_to_reports(uint16_t conn_handle, device_gatt_info_t *gatt)
{
    uint8_t value[2] = {0x01, 0x00}; /* Enable notifications */

    for (uint8_t i = 0; i < gatt->report_count; i++) {
        if (gatt->reports[i].subscribed) {
            continue;
        }

        if (gatt->reports[i].cccd_handle != 0) {
            int rc = ble_gattc_write_flat(conn_handle,
                                          gatt->reports[i].cccd_handle,
                                          value, sizeof(value),
                                          on_subscribe_write, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to subscribe to report %d: rc=%d", i, rc);
            } else {
                gatt->reports[i].subscribed = true;
                ESP_LOGI(TAG, "Subscribing to report chr val_handle=%d cccd=%d",
                         gatt->reports[i].chr_val_handle,
                         gatt->reports[i].cccd_handle);
            }
        } else {
            ESP_LOGW(TAG, "No CCCD for report chr val_handle=%d, trying direct subscribe",
                     gatt->reports[i].chr_val_handle);
            /* Try writing to val_handle + 1 (typical CCCD location) */
            int rc = ble_gattc_write_flat(conn_handle,
                                          gatt->reports[i].chr_val_handle + 1,
                                          value, sizeof(value),
                                          on_subscribe_write, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to subscribe (direct) to report %d: rc=%d", i, rc);
            } else {
                gatt->reports[i].cccd_handle = gatt->reports[i].chr_val_handle + 1;
                gatt->reports[i].subscribed = true;
            }
        }
    }
}

/* ----- Scan ----- */

static void start_scan(void)
{
    struct ble_gap_disc_params disc_params;
    int rc;

    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.itvl = 0x0030;   /* 30ms scan interval */
    disc_params.window = 0x0030; /* 30ms scan window */
    disc_params.filter_duplicates = 1;

    rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                      &disc_params, ble_central_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: rc=%d", rc);
        return;
    }

    scanning = true;
    ESP_LOGI(TAG, "BLE scan started");
}

void ble_central_start_scan(void)
{
    scan_result_count = 0;
    memset(scan_results, 0, sizeof(scan_results));
    start_scan();
}

void ble_central_stop_scan(void)
{
    int rc = ble_gap_disc_cancel();
    if (rc == 0) {
        scanning = false;
        ESP_LOGI(TAG, "BLE scan stopped");
    }
}

/* ----- Connect ----- */

static void attempt_reconnect(const input_device_t *dev, bool is_keyboard)
{
    if (dev->mac[0] == 0 && dev->mac[1] == 0 && dev->mac[2] == 0 &&
        dev->mac[3] == 0 && dev->mac[4] == 0 && dev->mac[5] == 0) {
        return;
    }

    ESP_LOGI(TAG, "Auto-reconnecting %s to %02x:%02x:%02x:%02x:%02x:%02x",
             is_keyboard ? "keyboard" : "mouse",
             dev->mac[5], dev->mac[4], dev->mac[3],
             dev->mac[2], dev->mac[1], dev->mac[0]);

    if (is_keyboard) {
        ble_central_connect_keyboard(dev->mac, dev->addr_type);
    } else {
        ble_central_connect_mouse(dev->mac, dev->addr_type);
    }
}

void ble_central_connect_keyboard(const uint8_t *addr, uint8_t addr_type)
{
    ble_addr_t peer_addr;
    int rc;

    if (keyboard_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Keyboard already connected");
        return;
    }

    memcpy(peer_addr.val, addr, 6);
    peer_addr.type = addr_type;

    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer_addr,
                         30000, /* 30s timeout */
                         NULL, /* default connection params */
                         ble_central_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to connect keyboard: rc=%d", rc);
        return;
    }

    /* Temporarily store the pending handle (will be confirmed in CONNECT event) */
    keyboard_conn_handle = 0xFFFF; /* sentinel for pending */
    ESP_LOGI(TAG, "Connecting to keyboard %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

void ble_central_connect_mouse(const uint8_t *addr, uint8_t addr_type)
{
    ble_addr_t peer_addr;
    int rc;

    if (mouse_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Mouse already connected");
        return;
    }

    memcpy(peer_addr.val, addr, 6);
    peer_addr.type = addr_type;

    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer_addr,
                         30000, /* 30s timeout */
                         NULL, /* default connection params */
                         ble_central_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to connect mouse: rc=%d", rc);
        return;
    }

    mouse_conn_handle = 0xFFFF; /* sentinel for pending */
    ESP_LOGI(TAG, "Connecting to mouse %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

/* ----- Status ----- */

bool ble_central_is_keyboard_connected(void)
{
    return keyboard_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
           keyboard_conn_handle != 0xFFFF;
}

bool ble_central_is_mouse_connected(void)
{
    return mouse_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
           mouse_conn_handle != 0xFFFF;
}

/* ----- Init ----- */

void ble_central_init(void)
{
    memset(scan_results, 0, sizeof(scan_results));
    scan_result_count = 0;
    scanning = false;
    keyboard_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    mouse_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    memset(&keyboard_disc, 0, sizeof(keyboard_disc));
    memset(&mouse_disc, 0, sizeof(mouse_disc));
    memset(&keyboard_gatt, 0, sizeof(keyboard_gatt));
    memset(&mouse_gatt, 0, sizeof(mouse_gatt));

    ESP_LOGI(TAG, "BLE central initialized");

    /* Attempt auto-connect to configured devices */
    const kvm_config_t *cfg = config_get();

    bool kb_nonzero = false;
    for (int i = 0; i < 6; i++) {
        if (cfg->keyboard.mac[i] != 0) { kb_nonzero = true; break; }
    }
    if (kb_nonzero) {
        ESP_LOGI(TAG, "Auto-connecting to configured keyboard");
        ble_central_connect_keyboard(cfg->keyboard.mac, cfg->keyboard.addr_type);
    }

    bool ms_nonzero = false;
    for (int i = 0; i < 6; i++) {
        if (cfg->mouse.mac[i] != 0) { ms_nonzero = true; break; }
    }
    if (ms_nonzero) {
        ESP_LOGI(TAG, "Auto-connecting to configured mouse");
        ble_central_connect_mouse(cfg->mouse.mac, cfg->mouse.addr_type);
    }
}
