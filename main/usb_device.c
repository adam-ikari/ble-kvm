#include "usb_device.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"

static const char *TAG = "usb_device";
static bool usb_mounted = false;

/* HID Report Descriptor: keyboard (Report ID 1) + mouse (Report ID 2) */
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(2)),
};

/* String descriptors */
static const char *hid_string_descriptor[5] = {
    "\x09\x04",     /* 0: English */
    "BLE-KVM",      /* 1: Manufacturer */
    "BLE-KVM HID",  /* 2: Product */
    "12345678",     /* 3: Serial */
    "BLE-KVM HID",  /* 4: HID interface */
};

/* Configuration descriptor: 1 config, 1 HID interface */
#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/* TinyUSB callbacks */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}

/* TinyUSB event callback */
static void tusb_event_cb(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        usb_mounted = true;
        ESP_LOGI(TAG, "USB device mounted");
        break;
    case TINYUSB_EVENT_DETACHED:
        usb_mounted = false;
        ESP_LOGI(TAG, "USB device unmounted");
        break;
    default:
        break;
    }
}

void usb_device_init(void)
{
    tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 1,
        },
        .descriptor = {
            .device = NULL,
            .qualifier = NULL,
            .string = hid_string_descriptor,
            .string_count = 5,
            .full_speed_config = hid_configuration_descriptor,
            .high_speed_config = NULL,
        },
        .event_cb = tusb_event_cb,
        .event_arg = NULL,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB HID device initialized");
}

bool usb_device_is_connected(void)
{
    return usb_mounted;
}

int usb_device_send_keyboard(const uint8_t *report, uint8_t len)
{
    if (!usb_mounted || !tud_hid_ready()) return -1;
    bool ok = tud_hid_report(1, report, len);
    return ok ? 0 : -1;
}

int usb_device_send_mouse(const uint8_t *report, uint8_t len)
{
    if (!usb_mounted || !tud_hid_ready()) return -1;
    bool ok = tud_hid_report(2, report, len);
    return ok ? 0 : -1;
}
