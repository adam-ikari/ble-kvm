#pragma once

typedef enum {
    IND_PC1_ACTIVE,
    IND_PC2_ACTIVE,
    IND_PC3_ACTIVE,
    IND_NO_PC,
    IND_PAIRING,
    IND_VOICE_RECORDING,
    IND_FACTORY_WARN,
    IND_SLEEP,            /* device in low-power sleep, LED breathing */
} indicator_state_t;

void indicator_init(void);
void indicator_set_state(indicator_state_t state);
