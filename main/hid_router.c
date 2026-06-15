#include "hid_router.h"
#include "switch_manager.h"
#include "ble_peripheral.h"
#include "config_manager.h"
#include "event_bus.h"
#include "host/ble_hs.h"
#include "esp_log.h"
#if HAS_USB
#include "usb_device.h"
#endif

static const char *TAG = "hid_router";

static void hid_keyboard_data_handler(void *arg, esp_event_base_t base,
                                       int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    hid_router_forward_keyboard(evt->data, evt->len);
}

static void hid_mouse_data_handler(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    app_evt_hid_data_t *evt = (app_evt_hid_data_t *)event_data;
    hid_router_forward_mouse(evt->data, evt->len);
}

void hid_router_init(void)
{
    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_KEYBOARD_DATA, hid_keyboard_data_handler, NULL);
    APP_EVENT_SUBSCRIBE(APP_EVENT_HID_MOUSE_DATA, hid_mouse_data_handler, NULL);
    ESP_LOGI(TAG, "HID router initialized");
}

void hid_router_forward_keyboard(const uint8_t *report, uint8_t len)
{
    const kvm_config_t *cfg = config_get();

    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        int rc = usb_device_send_keyboard(report, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "USB keyboard forward failed: rc=%d", rc);
        }
#endif
    } else {
        uint16_t conn_handle = switch_manager_get_active_conn_handle();
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            return;
        }
        int rc = ble_peripheral_send_hid_report(conn_handle, 1, report, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE keyboard forward failed: rc=%d", rc);
        }
    }

    APP_EVENT_POST(APP_EVENT_HID_ACTIVITY, NULL, 0);
}

void hid_router_forward_mouse(const uint8_t *report, uint8_t len)
{
    const kvm_config_t *cfg = config_get();

    if (cfg->active_pc == 3 && cfg->usb_mode == USB_MODE_DEVICE) {
#if HAS_USB
        int rc = usb_device_send_mouse(report, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "USB mouse forward failed: rc=%d", rc);
        }
#endif
    } else {
        uint16_t conn_handle = switch_manager_get_active_conn_handle();
        if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            return;
        }
        int rc = ble_peripheral_send_hid_report(conn_handle, 2, report, len);
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE mouse forward failed: rc=%d", rc);
        }
    }

    APP_EVENT_POST(APP_EVENT_HID_ACTIVITY, NULL, 0);
}

void hid_router_on_usb_keyboard(const uint8_t *report, uint8_t len)
{
    hid_router_forward_keyboard(report, len);
}

void hid_router_on_usb_mouse(const uint8_t *report, uint8_t len)
{
    hid_router_forward_mouse(report, len);
}
