#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "board.h"

/* ── Battery / PMIC ──────────────────────────────────────────────── */
void power_manager_init(i2c_master_bus_handle_t bus);
bool power_manager_is_charging(void);
uint8_t power_manager_get_battery_percent(void);
uint16_t power_manager_get_battery_voltage_mv(void);
bool power_manager_is_usb_powered(void);

/* ── Sleep state machine (battery hardware only) ─────────────────── */
typedef enum {
    PM_STATE_ACTIVE,       /* fully awake */
    PM_STATE_SCREEN_OFF,   /* backlight off, LCD frozen, everything else running */
    PM_STATE_SLEEP,        /* light sleep: BLE advertising, LED breathing, rest off */
} pm_sleep_state_t;

#if HAS_BATTERY
void pm_sleep_init(void);
pm_sleep_state_t pm_sleep_get_state(void);
void pm_sleep_enter_force(void);        /* force sleep — not used in v1.0 */
#else
static inline void pm_sleep_init(void) {}
static inline pm_sleep_state_t pm_sleep_get_state(void) { return PM_STATE_ACTIVE; }
static inline void pm_sleep_enter_force(void) {}
#endif
