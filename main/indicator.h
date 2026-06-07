#pragma once

typedef enum {
    IND_PC1_ACTIVE,
    IND_PC2_ACTIVE,
    IND_NO_PC,
    IND_PAIRING,
} indicator_state_t;

void indicator_init(void);
void indicator_set_state(indicator_state_t state);
