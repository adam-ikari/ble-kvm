#pragma once

#include <stddef.h>
#include <stdbool.h>

void web_log_init(void);
void web_log_enable(void);
void web_log_disable(void);
bool web_log_is_enabled(void);

/* SSE: broadcast each new log line to connected web clients */
void web_log_register_sse_broadcast(void (*broadcast)(const char *event, const char *data));