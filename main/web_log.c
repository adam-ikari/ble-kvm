#include "web_log.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "web_log";
static bool enabled = false;
static vprintf_like_t original_vprintf = NULL;
static void (*sse_broadcast_fn)(const char *event, const char *data) = NULL;

static int web_log_vprintf(const char *fmt, va_list ap)
{
    /* Send to SSE clients as individual lines */
    char tmp[256];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (len > 0 && sse_broadcast_fn && enabled) {
        /* Strip trailing newline — client will add it back */
        if (tmp[len - 1] == '\n') {
            tmp[len - 1] = '\0';
        }
        /* Replace embedded newlines with \r for SSE multi-line data */
        for (int i = 0; i < len; i++) {
            if (tmp[i] == '\n') tmp[i] = '\r';
        }
        sse_broadcast_fn("log", tmp);
    }

    /* Also call original vprintf to keep serial output */
    if (original_vprintf) {
        va_list ap2;
        va_copy(ap2, ap);
        original_vprintf(fmt, ap2);
        va_end(ap2);
    }

    return len;
}

void web_log_init(void)
{
    original_vprintf = esp_log_set_vprintf(web_log_vprintf);
    /* Disabled by default — enable only sends SSE, no buffer needed */
    ESP_LOGI(TAG, "Web log initialized");
}

void web_log_enable(void)
{
    if (!enabled) {
        enabled = true;
        ESP_LOGI(TAG, "Web log enabled");
    }
}

void web_log_disable(void)
{
    if (enabled) {
        enabled = false;
        ESP_LOGI(TAG, "Web log disabled");
    }
}

bool web_log_is_enabled(void)
{
    return enabled;
}

void web_log_register_sse_broadcast(void (*broadcast)(const char *event, const char *data))
{
    sse_broadcast_fn = broadcast;
}