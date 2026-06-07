idf_component_register(
    SRCS "main.c" "led_controller.c" "config_manager.c" "ble_peripheral.c" "switch_manager.c" "hid_router.c" "ble_central.c" "wifi_manager.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_wifi esp_netif esp_event json esp_http_server
)
