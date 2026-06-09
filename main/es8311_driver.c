#include "es8311_driver.h"
#include "board.h"

#if HAS_VOICE_INPUT

#include "esp_log.h"

static const char *TAG = "es8311";
static i2c_master_dev_handle_t es8311_dev = NULL;

/* ES8311 register addresses */
#define ES8311_RESET_REG    0x00
#define ES8311_CLKMGR1      0x01
#define ES8311_CLKMGR2      0x02
#define ES8311_CLKMGR3      0x03
#define ES8311_ADC1         0x05
#define ES8311_ADC2         0x06
#define ES8311_ADC3         0x07
#define ES8311_ADC4         0x08
#define ES8311_ADC5         0x09
#define ES8311_ADC6         0x0A
#define ES8311_SYSTEM       0x0C
#define ES8311_PDN          0x0D
#define ES8311_GPIO         0x44

/* Reset values */
#define ES8311_RESET_TRIGGER  0x80
#define ES8311_RESET_RELEASE  0x00

/* Power down bits */
#define ES8311_PDN_PDBIAS   (1 << 4)
#define ES8311_PDN_PDDAC    (1 << 3)
#define ES8311_PDN_PDDRV    (1 << 2)
#define ES8311_PDN_PDAVDD1  (1 << 1)
#define ES8311_PDN_PDAVDD2  (1 << 0)

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(es8311_dev, buf, 2, 100);
}

void es8311_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &es8311_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 device: %s", esp_err_to_name(err));
        return;
    }

    /* Reset the codec */
    es8311_write_reg(ES8311_RESET_REG, ES8311_RESET_TRIGGER);
    es8311_write_reg(ES8311_RESET_REG, ES8311_RESET_RELEASE);

    /* Clock manager: ADC clock from MCLK */
    es8311_write_reg(ES8311_CLKMGR1, 0x00);  /* MCLK as codec clock source */
    es8311_write_reg(ES8311_CLKMGR2, 0x00);  /* ADC clock divider = 1 */
    es8311_write_reg(ES8311_CLKMGR3, 0x00);  /* DAC clock divider = 1 */

    /* System: select MIC1p/MIC1n input */
    es8311_write_reg(ES8311_SYSTEM, 0x10);   /* MIC1p/MIC1n input, ADC data output */

    /* ADC configuration */
    es8311_write_reg(ES8311_ADC1, 0x00);     /* ADC power up */
    es8311_write_reg(ES8311_ADC2, 0x00);     /* ADC gain = 0dB (use set_mic_gain to adjust) */
    es8311_write_reg(ES8311_ADC3, 0x00);     /* ADC HPF enable */
    es8311_write_reg(ES8311_ADC4, 0x00);     /* ADC mixer */
    es8311_write_reg(ES8311_ADC5, 0x00);     /* ADC EQ */
    es8311_write_reg(ES8311_ADC6, 0x00);     /* ADC alc */

    /* GPIO: ADCDATA output */
    es8311_write_reg(ES8311_GPIO, 0x04);     /* GPIO as ADCDATA output */

    /* Power down: power up all */
    es8311_write_reg(ES8311_PDN, 0x00);      /* Power up all blocks */

    ESP_LOGI(TAG, "ES8311 initialized at addr 0x%02X", ES8311_I2C_ADDR);
}

void es8311_set_mic_gain(uint8_t gain)
{
    if (!es8311_dev) return;

    /* Gain range: 0-255 maps to -95.5dB to +32dB in 0.5dB steps */
    /* ADC2 register bits [7:0] control mic gain */
    es8311_write_reg(ES8311_ADC2, gain);
    ESP_LOGD(TAG, "Set mic gain to %d", gain);
}

void es8311_deinit(void)
{
    if (es8311_dev) {
        /* Power down codec */
        es8311_write_reg(ES8311_PDN,
            ES8311_PDN_PDBIAS | ES8311_PDN_PDDAC | ES8311_PDN_PDDRV |
            ES8311_PDN_PDAVDD1 | ES8311_PDN_PDAVDD2);

        i2c_master_bus_rm_device(es8311_dev);
        es8311_dev = NULL;
        ESP_LOGI(TAG, "ES8311 deinitialized");
    }
}

#endif /* HAS_VOICE_INPUT */
