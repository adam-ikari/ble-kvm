#include "usb_host.h"
#include "event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

static const char *TAG = "usb_host";
static bool kb_connected = false;
static bool ms_connected = false;
static hid_host_device_handle_t kb_handle = NULL;
static hid_host_device_handle_t ms_handle = NULL;
static TaskHandle_t usb_host_task_handle = NULL;

/* USB Host library task */
static void usb_host_lib_task(void *arg)
{
    uint32_t event_flags;
    while (1) {
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

/* HID interface event callback */
static void hid_interface_cb(hid_host_device_handle_t dev,
                              const hid_host_interface_event_t event, void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[16];
        size_t data_len = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &data_len);
        if (err != ESP_OK) return;

        hid_host_dev_params_t params;
        hid_host_device_get_params(dev, &params);

        if (params.proto == HID_PROTOCOL_KEYBOARD) {
            app_evt_hid_data_t hid_evt = { .data = data, .len = (uint8_t)data_len };
            APP_EVENT_POST(APP_EVENT_HID_KEYBOARD_DATA, &hid_evt, sizeof(hid_evt));
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            app_evt_hid_data_t hid_evt = { .data = data, .len = (uint8_t)data_len };
            APP_EVENT_POST(APP_EVENT_HID_MOUSE_DATA, &hid_evt, sizeof(hid_evt));
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hid_host_dev_params_t params;
        hid_host_device_get_params(dev, &params);

        if (params.proto == HID_PROTOCOL_KEYBOARD) {
            kb_connected = false;
            kb_handle = NULL;
            ESP_LOGI(TAG, "USB keyboard disconnected");
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            ms_connected = false;
            ms_handle = NULL;
            ESP_LOGI(TAG, "USB mouse disconnected");
        }
        hid_host_device_close(dev);
    }
}

/* HID driver event callback */
static void hid_driver_cb(hid_host_device_handle_t dev,
                           const hid_host_driver_event_t event, void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params;
    hid_host_device_get_params(dev, &params);

    hid_host_device_config_t dev_cfg = {
        .callback = hid_interface_cb,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_device_open(dev, &dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open HID device: %s", esp_err_to_name(err));
        return;
    }

    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        kb_connected = true;
        kb_handle = dev;
        ESP_LOGI(TAG, "USB keyboard connected");
    } else if (params.proto == HID_PROTOCOL_MOUSE) {
        ms_connected = true;
        ms_handle = dev;
        ESP_LOGI(TAG, "USB mouse connected");
    }

    hid_host_device_start(dev);
}

void usb_host_init(void)
{
    /* Install USB Host library */
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    /* Create USB host library task on core 0 */
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host_lib", 4096, NULL, 5,
                             &usb_host_task_handle, 0);

    /* Wait for USB host library to be ready */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Install HID host driver */
    hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));

    ESP_LOGI(TAG, "USB HID host initialized");
}

bool usb_host_is_keyboard_connected(void)
{
    return kb_connected;
}

bool usb_host_is_mouse_connected(void)
{
    return ms_connected;
}