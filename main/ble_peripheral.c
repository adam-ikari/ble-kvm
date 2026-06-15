#include "ble_peripheral.h"
#include "config_manager.h"
#include "event_bus.h"
#include "indicator.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/dis/ble_svc_dis.h"
#include "services/bas/ble_svc_bas.h"

static const char *TAG = "ble_peripheral";

#define MAX_PC_CONNECTIONS 2

/* HID Report IDs */
#define HID_REPORT_ID_KEYBOARD  1
#define HID_REPORT_ID_MOUSE     2
#define HID_REPORT_ID_CONSUMER  3

/* GATT Service UUIDs */
#define BLE_SVC_HID_UUID16              0x1812
#define BLE_SVC_HID_CHR_PROTOCOL_MODE   0x2A4E
#define BLE_SVC_HID_CHR_REPORT_MAP      0x2A4B
#define BLE_SVC_HID_CHR_REPORT          0x2A4D
#define BLE_SVC_HID_CHR_INFO            0x2A4A
#define BLE_SVC_HID_CHR_CONTROL_POINT   0x2A4C

/* Protocol Mode values */
#define HID_PROTOCOL_MODE_BOOT       0x00
#define HID_PROTOCOL_MODE_REPORT     0x01

/* HID Information: version 2.0 (0x0200), country code 0, flags 0 */
static const uint8_t hid_information[] = {0x00, 0x02};

/* HID Report Map: keyboard (Report ID 1) + mouse (Report ID 2) + consumer (Report ID 3) */
static const uint8_t hid_report_map[] = {
    /* Keyboard (Report ID 1) */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    /* Mouse (Report ID 2) */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
    /* Consumer Control (Report ID 3) */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x03,
    0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

/* Per-PC connection tracking */
typedef struct {
    uint8_t pc_id;
    uint16_t conn_handle;
    bool connected;
} pc_conn_t;

static pc_conn_t pc_conns[MAX_PC_CONNECTIONS] = {
    { .pc_id = 0, .conn_handle = 0, .connected = false },
    { .pc_id = 1, .conn_handle = 0, .connected = false },
};

static bool pairing_mode = false;
static esp_timer_handle_t pairing_timer = NULL;

static void pairing_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "Pairing mode timeout");
    ble_peripheral_exit_pairing_mode();
}

void ble_peripheral_enter_pairing_mode(void)
{
    if (pairing_mode) return;
    pairing_mode = true;
    ESP_LOGI(TAG, "Pairing mode entered (60s timeout)");

    /* Start advertising if not already */
    ble_peripheral_start_advertising();

    /* Set LED to pairing indicator */
    indicator_set_state(IND_PAIRING);

    /* Start 60s timeout */
    if (pairing_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = pairing_timeout_cb,
            .name = "pairing_to",
        };
        esp_timer_create(&args, &pairing_timer);
    }
    esp_timer_start_once(pairing_timer, 60000000); /* 60s */
}

void ble_peripheral_exit_pairing_mode(void)
{
    if (!pairing_mode) return;
    pairing_mode = false;
    ESP_LOGI(TAG, "Pairing mode exited");
    esp_timer_stop(pairing_timer);
    /* LED will be restored by switch_manager's update_led_state */
}

bool ble_peripheral_is_pairing_mode(void)
{
    return pairing_mode;
}

/* Storage for characteristic value handles (filled at registration time) */
static uint16_t keyboard_report_val_handle;
static uint16_t mouse_report_val_handle;
static uint16_t consumer_report_val_handle;

/* Forward declarations */
static int ble_hid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);

/* ----- GATT Service Table ----- */

/* Keyboard Report CCC descriptor */
static struct ble_gatt_dsc_def hid_dsc_keyboard_ccc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
        .access_cb = ble_hid_access_cb,
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
    },
    { 0 },
};

/* Mouse Report CCC descriptor */
static struct ble_gatt_dsc_def hid_dsc_mouse_ccc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
        .access_cb = ble_hid_access_cb,
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
    },
    { 0 },
};

/* Consumer Control Report CCC descriptor */
static struct ble_gatt_dsc_def hid_dsc_consumer_ccc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
        .access_cb = ble_hid_access_cb,
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
    },
    { 0 },
};

/* HID Service characteristic definitions */
static const struct ble_gatt_chr_def hid_svc_chrs[] = {
    /* Protocol Mode */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_PROTOCOL_MODE),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    /* Report Map */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_REPORT_MAP),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    /* Keyboard Report (Report ID 1) */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_REPORT),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &keyboard_report_val_handle,
        .descriptors = hid_dsc_keyboard_ccc,
    },
    /* Mouse Report (Report ID 2) */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_REPORT),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &mouse_report_val_handle,
        .descriptors = hid_dsc_mouse_ccc,
    },
    /* Consumer Control Report (Report ID 3) */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_REPORT),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &consumer_report_val_handle,
        .descriptors = hid_dsc_consumer_ccc,
    },
    /* HID Information */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_INFO),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    /* HID Control Point */
    {
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_CHR_CONTROL_POINT),
        .access_cb = ble_hid_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    { 0 },
};

/* HID Service definition */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    /* HID Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_HID_UUID16),
        .characteristics = hid_svc_chrs,
    },
    { 0 },
};

/* ----- GATT Access Callback ----- */

static int ble_hid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_REPORT_MAP) {
            rc = os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_PROTOCOL_MODE) {
            uint8_t mode = HID_PROTOCOL_MODE_REPORT;
            rc = os_mbuf_append(ctxt->om, &mode, sizeof(mode));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_INFO) {
            rc = os_mbuf_append(ctxt->om, hid_information, sizeof(hid_information));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_REPORT) {
            /* Read of report characteristic returns empty/zero */
            uint8_t zero = 0;
            rc = os_mbuf_append(ctxt->om, &zero, sizeof(zero));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_PROTOCOL_MODE) {
            /* Accept but stay in Report Protocol Mode */
            ESP_LOGD(TAG, "Protocol mode write (ignored, staying in Report mode)");
            return 0;
        }
        if (ble_uuid_u16(ctxt->chr->uuid) == BLE_SVC_HID_CHR_CONTROL_POINT) {
            /* Accept suspend/resume, we can ignore */
            ESP_LOGD(TAG, "HID Control Point write");
            return 0;
        }
        break;

    case BLE_GATT_ACCESS_OP_READ_DSC:
        /* CCC descriptor reads are handled by NimBLE internally */
        break;

    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        /* CCC descriptor writes are handled by NimBLE internally */
        break;

    default:
        break;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ----- GAP Event Handler ----- */

static int find_free_pc_slot(void)
{
    for (int i = 0; i < MAX_PC_CONNECTIONS; i++) {
        if (!pc_conns[i].connected) {
            return i;
        }
    }
    return -1;
}

static int find_pc_slot_by_handle(uint16_t conn_handle)
{
    for (int i = 0; i < MAX_PC_CONNECTIONS; i++) {
        if (pc_conns[i].connected && pc_conns[i].conn_handle == conn_handle) {
            return i;
        }
    }
    return -1;
}

static void restart_advertising_if_needed(void)
{
    /* Check if all PC slots are filled */
    for (int i = 0; i < MAX_PC_CONNECTIONS; i++) {
        if (!pc_conns[i].connected) {
            ble_peripheral_start_advertising();
            return;
        }
    }
    ESP_LOGI(TAG, "All PC slots connected, not restarting advertising");
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_CONNECT; status=%d", event->connect.status);

        if (event->connect.status == 0) {
            /* Check if we should accept this connection */
            bool accept = pairing_mode;

            /* Also accept if this device matches a previously paired PC */
            if (!accept) {
                const kvm_config_t *cfg = config_get();
                struct ble_gap_conn_desc desc;
                int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
                if (rc == 0) {
                    for (int i = 0; i < MAX_PC_COUNT - 1; i++) {  /* PC1 and PC2 only */
                        if (memcmp(cfg->pcs[i].identity_addr, desc.peer_id_addr.val, 6) == 0) {
                            /* Connecting device matches a saved PC identity address — accept */
                            accept = true;
                            break;
                        }
                    }
                }
            }

            if (!accept) {
                ESP_LOGW(TAG, "Not in pairing mode, rejecting connection");
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                break;
            }

            /* Connection successful */
            int slot = find_free_pc_slot();
            if (slot >= 0) {
                pc_conns[slot].conn_handle = event->connect.conn_handle;
                pc_conns[slot].connected = true;
                ESP_LOGI(TAG, "PC %d connected (handle=%d)", slot + 1, event->connect.conn_handle);

                app_evt_pc_connected_t evt = {
                    .pc_id = slot + 1,
                    .conn_handle = event->connect.conn_handle,
                };
                APP_EVENT_POST(APP_EVENT_PC_CONNECTED, &evt, sizeof(evt));

                /* Exit pairing mode after successful connection */
                if (pairing_mode) {
                    ble_peripheral_exit_pairing_mode();
                }

                /* Restart advertising if not all PCs connected */
                restart_advertising_if_needed();
            } else {
                ESP_LOGW(TAG, "No free PC slot, disconnecting");
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Connection failed; restart advertising */
            ble_peripheral_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_DISCONNECT; reason=%d", event->disconnect.reason);

        {
            int slot = find_pc_slot_by_handle(event->disconnect.conn.conn_handle);
            if (slot >= 0) {
                ESP_LOGI(TAG, "PC %d disconnected", slot + 1);
                pc_conns[slot].connected = false;
                pc_conns[slot].conn_handle = 0;

                app_evt_pc_disconnected_t evt = { .pc_id = slot + 1 };
                APP_EVENT_POST(APP_EVENT_PC_DISCONNECTED, &evt, sizeof(evt));
            }
        }

        /* Restart advertising for reconnect */
        ble_peripheral_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE_GAP_EVENT_ADV_COMPLETE; reason=%d",
                 event->adv_complete.reason);
        ble_peripheral_start_advertising();
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

/* ----- Advertising ----- */

void ble_peripheral_start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));

    /* General discoverable + BR/EDR not supported */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Include device name */
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* Appearance: HID Generic (0x03C0) */
    fields.appearance = 0x03C0;
    fields.appearance_is_present = 1;

    /* Include TX power level */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting adv fields: rc=%d", rc);
        return;
    }

    /* Set advertising parameters: fast connect */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x0020; /* 20ms */
    adv_params.itvl_max = 0x0040; /* 40ms */

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting advertising: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started");
}

void ble_peripheral_start_directed_advertising(const pc_device_t *pc)
{
    struct ble_gap_adv_params adv_params;
    ble_addr_t peer_addr;
    int rc;

    if (pc == NULL) {
        return;
    }

    memcpy(peer_addr.val, pc->identity_addr, 6);
    peer_addr.type = pc->addr_type;

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_DIR;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.high_duty_cycle = 1;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, &peer_addr, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting directed advertising: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Directed advertising started to PC %d", pc->pc_id);
}

void ble_peripheral_stop_advertising(void)
{
    ble_gap_adv_stop();
    ESP_LOGI(TAG, "Advertising stopped");
}

/* ----- Connection Query ----- */

uint16_t ble_peripheral_get_conn_handle(uint8_t pc_id)
{
    if (pc_id >= MAX_PC_CONNECTIONS) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    if (!pc_conns[pc_id].connected) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    return pc_conns[pc_id].conn_handle;
}

bool ble_peripheral_is_pc_connected(uint8_t pc_id)
{
    if (pc_id >= MAX_PC_CONNECTIONS) {
        return false;
    }
    return pc_conns[pc_id].connected;
}

/* ----- Send HID Report ----- */

int ble_peripheral_send_hid_report(uint16_t conn_handle, uint8_t report_id,
                                    const uint8_t *data, uint8_t len)
{
    if (conn_handle == 0) {
        return BLE_HS_ENOTCONN;
    }

    /* Determine which report characteristic handle to use based on report_id */
    uint16_t attr_handle;
    if (report_id == HID_REPORT_ID_KEYBOARD) {
        attr_handle = keyboard_report_val_handle;
    } else if (report_id == HID_REPORT_ID_MOUSE) {
        attr_handle = mouse_report_val_handle;
    } else if (report_id == HID_REPORT_ID_CONSUMER) {
        attr_handle = consumer_report_val_handle;
    } else {
        ESP_LOGE(TAG, "Unknown report ID: %d", report_id);
        return BLE_HS_EINVAL;
    }

    if (attr_handle == 0) {
        ESP_LOGE(TAG, "Report handle not resolved for ID %d", report_id);
        return BLE_HS_EINVAL;
    }

    /* Build the report data: report_id byte + data.
     * Use a fixed-size buffer (max HID report is 64 bytes + 1 report_id). */
    uint8_t report_data[65];
    if (len > 64) {
        ESP_LOGE(TAG, "Report data too long: %d bytes", len);
        return BLE_HS_EINVAL;
    }
    report_data[0] = report_id;
    memcpy(&report_data[1], data, len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report_data, 1 + len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for HID report");
        return BLE_HS_ENOMEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to send HID report: rc=%d", rc);
    }

    return rc;
}

int ble_peripheral_send_consumer_key(uint16_t conn_handle, uint16_t usage_code)
{
    uint8_t data[2] = {usage_code & 0xFF, (usage_code >> 8) & 0xFF};
    return ble_peripheral_send_hid_report(conn_handle, HID_REPORT_ID_CONSUMER, data, 2);
}

/* ----- Init ----- */

void ble_peripheral_init(void)
{
    int rc;

    /* Set device name */
    uint8_t mac[6];
    char default_name[16];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(default_name, sizeof(default_name), "KVM-%02X%02X", mac[4], mac[5]);

    const kvm_config_t *cfg = config_get();
    if (cfg->device_name[0] != '\0') {
        ble_svc_gap_device_name_set(cfg->device_name);
    } else {
        ble_svc_gap_device_name_set(default_name);
    }

    /* Configure host settings */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    /* Add required services for HID */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_dis_init();
    ble_svc_bas_init();

    /* Configure Device Information Service */
    ble_svc_dis_manufacturer_name_set(cfg->device_name[0] ? cfg->device_name : "BLE-KVM");

    /* Configure GATT server */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: rc=%d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE peripheral initialized");
}
