idf_component_register(
    SRCS "main.c" "led_controller.c" "config_manager.c" "ble_peripheral.c" "switch_manager.c" "hid_router.c" "ble_central.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio
)
