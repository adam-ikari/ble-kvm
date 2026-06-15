#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "event_bus";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static esp_event_loop_handle_t app_loop = NULL;

void event_bus_init(void)
{
    esp_event_loop_args_t loop_args = {
        .queue_size = 32,
        .task_name = "app_evt",
        .task_priority = 5,
        .task_stack_size = 4096,
        .task_core_id = 0,
    };

    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &app_loop));
    ESP_LOGI(TAG, "Application event loop created");
}

esp_event_loop_handle_t app_event_loop_handle(void)
{
    return app_loop;
}
