#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_PC_COUNT 3
#define DEVICE_NAME_MAX 32
#define AUTH_TOKEN_LEN 9

#define USB_MODE_DISABLED  0
#define USB_MODE_DEVICE    1
#define USB_MODE_HOST      2

typedef struct {
    uint8_t identity_addr[6];
    uint8_t addr_type;
    uint8_t pc_id;
    char name[DEVICE_NAME_MAX];
    uint16_t conn_handle;
    bool connected;
} pc_device_t;

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    bool is_combo;
    char name[DEVICE_NAME_MAX];
    bool connected;
} input_device_t;

typedef struct {
    pc_device_t pcs[MAX_PC_COUNT];
    input_device_t keyboard;
    input_device_t mouse;
    uint8_t active_pc;
    uint8_t usb_mode;               /* 0=disabled, 1=device, 2=host */
    char auth_token[AUTH_TOKEN_LEN];
    char wifi_ssid[33];
    char wifi_password[65];
    bool wifi_enabled;
    bool anti_idle_enabled;
    uint16_t anti_idle_interval_sec;
    uint8_t input_mode;            /* 0=KVM, 1=PPT/Air Mouse */
    uint8_t air_mouse_sensitivity; /* 1-10, default 5 */
} kvm_config_t;

void config_manager_init(void);
const kvm_config_t *config_get(void);
kvm_config_t *config_get_mutable(void);
void config_save_pcs(void);
void config_save_input_devices(void);
void config_save_active_pc(void);
void config_save_auth_token(void);
void config_save_wifi(void);
void config_generate_auth_token(void);
void config_save_anti_idle(void);
void config_save_input_mode(void);
void config_save_usb_mode(void);
