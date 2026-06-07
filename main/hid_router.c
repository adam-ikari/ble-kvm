#include "hid_router.h"
#include "switch_manager.h"
#include "ble_peripheral.h"
#include "host/ble_hs.h"
#include "esp_log.h"

static const char *TAG = "hid_router";

void hid_router_init(void)
{
    ESP_LOGI(TAG, "HID router initialized");
}

void hid_router_forward_keyboard(const uint8_t *report, uint8_t len)
{
    uint16_t conn_handle = switch_manager_get_active_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int rc = ble_peripheral_send_hid_report(conn_handle, 1, report, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "Keyboard forward failed: rc=%d", rc);
    }
}

void hid_router_forward_mouse(const uint8_t *report, uint8_t len)
{
    uint16_t conn_handle = switch_manager_get_active_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int rc = ble_peripheral_send_hid_report(conn_handle, 2, report, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "Mouse forward failed: rc=%d", rc);
    }
}
