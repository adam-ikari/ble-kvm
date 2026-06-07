#pragma once

#include "driver/gpio.h"

#ifdef BOARD_M5STICKS3
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_11
  #define BUTTON_SECONDARY_GPIO  GPIO_NUM_12
  #define HAS_SECONDARY_BUTTON   1
  #define HAS_TFT_DISPLAY        1
  #define TFT_MOSI_GPIO          GPIO_NUM_39
  #define TFT_SCLK_GPIO          GPIO_NUM_40
  #define TFT_DC_GPIO            GPIO_NUM_45
  #define TFT_CS_GPIO            GPIO_NUM_41
  #define TFT_RST_GPIO           GPIO_NUM_21
  #define TFT_BL_GPIO            GPIO_NUM_38
  #define TFT_WIDTH              135
  #define TFT_HEIGHT             240
  #define TFT_OFFSET_X           52
  #define TFT_OFFSET_Y           40
  #define HAS_BATTERY            1
  #define PMIC_I2C_ADDR          0x6e
  #define I2C_SDA_GPIO           GPIO_NUM_47
  #define I2C_SCL_GPIO           GPIO_NUM_48
  #define HAS_RGB_LED            0
  #define HAS_GPIO_LED           0

#elif defined(BOARD_M5STAMPS3)
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  #define HAS_RGB_LED            1
  #define RGB_LED_GPIO           GPIO_NUM_27
  #define HAS_GPIO_LED           0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0

#else
  #define BUTTON_SWITCH_GPIO     GPIO_NUM_0
  #define HAS_SECONDARY_BUTTON   0
  #define HAS_GPIO_LED           1
  #define LED1_GPIO              GPIO_NUM_2
  #define LED2_GPIO              GPIO_NUM_1
  #define HAS_RGB_LED            0
  #define HAS_TFT_DISPLAY        0
  #define HAS_BATTERY            0
#endif
