#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

typedef enum {
    INPUT_MODE_KVM = 0,
    INPUT_MODE_PPT_AIR = 1,
} input_mode_t;

#if HAS_INPUT_MODES
void input_mode_init(void);
input_mode_t input_mode_get(void);
void input_mode_set(input_mode_t mode);
void input_mode_cycle(void);

void input_mode_on_primary_button(void);
void input_mode_on_secondary_button(void);
#else
static inline void input_mode_init(void) {}
static inline input_mode_t input_mode_get(void) { return INPUT_MODE_KVM; }
static inline void input_mode_set(input_mode_t m) { (void)m; }
static inline void input_mode_cycle(void) {}
static inline void input_mode_on_primary_button(void) {}
static inline void input_mode_on_secondary_button(void) {}
#endif
