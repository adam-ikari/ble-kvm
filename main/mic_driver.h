#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "board.h"

#if HAS_VOICE_INPUT

void mic_driver_init(void);
void mic_driver_start(void);
void mic_driver_stop(void);
int  mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms);
bool mic_driver_is_running(void);

#else

static inline void mic_driver_init(void) {}
static inline void mic_driver_start(void) {}
static inline void mic_driver_stop(void) {}
static inline int  mic_driver_read(void *buf, size_t len, size_t *bytes_read, unsigned int timeout_ms) { (void)buf; (void)len; (void)bytes_read; (void)timeout_ms; return -1; }
static inline bool mic_driver_is_running(void) { return false; }

#endif
