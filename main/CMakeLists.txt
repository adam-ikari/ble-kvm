if(NOT BOARD)
    set(BOARD "$ENV{BOARD}")
endif()

set(COMPONENT_SRCS
    "main.c" "config_manager.c" "ble_peripheral.c"
    "switch_manager.c" "hid_router.c" "ble_central.c"
    "wifi_manager.c" "web_server.c"
    "indicator.c" "anti_idle.c"
)

set(EXTRA_REQUIRES "")

if(BOARD STREQUAL "m5sticks3")
    list(APPEND COMPONENT_SRCS "tft_display.c" "power_manager.c" "input_mode.c" "imu_driver.c")
    list(APPEND EXTRA_REQUIRES "esp_lcd" "esp_driver_i2c")
elseif(BOARD STREQUAL "m5stamps3")
    list(APPEND COMPONENT_SRCS "rgb_led.c")
    list(APPEND EXTRA_REQUIRES "esp_driver_rmt")
else()
    list(APPEND COMPONENT_SRCS "gpio_led.c")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_wifi esp_netif
             esp_event json esp_http_server ${EXTRA_REQUIRES}
)

if(BOARD STREQUAL "m5sticks3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STICKS3)
    target_link_libraries(${COMPONENT_LIB} PRIVATE "-lm")
elseif(BOARD STREQUAL "m5stamps3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_M5STAMPS3)
endif()
