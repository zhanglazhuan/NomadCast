/*
 * ES8156 Audio DAC Driver — ported from Everest 8051 reference (ES8156.C)
 *
 * Replaces:
 *   I2CWRNBYTE_CODEC(reg, val) → ESP-IDF i2c_master_transmit()
 *   DELAY_MS(ms)               → vTaskDelay(pdMS_TO_TICKS(ms))
 *
 * Reference values (from ES8156.C macros):
 *   Ratio=256, Format=NORMAL_I2S, Format_Len=16bit, SCLK_DIV=4
 *   VDDA=3.3V, DAC_Volume=191 (0dB), Slave Mode
 */

#include "es8156.h"
#include "es8156_regs.h"
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ES8156";

#define ES8156_TIMEOUT_MS  100

/* ========================================================================
 * Internal handle
 * ======================================================================== */

typedef struct es8156 {
    i2c_master_dev_handle_t dev_handle;
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_address;
} es8156_t;

/* ========================================================================
 * Low-level I2C
 * ======================================================================== */

esp_err_t es8156_write_reg(es8156_handle_t h, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(h->dev_handle, buf, sizeof(buf), ES8156_TIMEOUT_MS);
}

esp_err_t es8156_read_reg(es8156_handle_t h, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(h->dev_handle, &reg, 1, value, 1, ES8156_TIMEOUT_MS);
}

/* ========================================================================
 * Init / Deinit
 * ======================================================================== */

esp_err_t es8156_initialize(const es8156_config_t *cfg, es8156_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && out_handle, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(cfg->i2c_bus, ESP_ERR_INVALID_ARG, TAG, "null i2c_bus");

    // Probe device on I2C bus
    ESP_RETURN_ON_ERROR(
        i2c_master_probe(cfg->i2c_bus, cfg->i2c_address, ES8156_TIMEOUT_MS),
        TAG, "ES8156 not found at addr 0x%02X", cfg->i2c_address);

    // Allocate handle
    es8156_t *h = calloc(1, sizeof(es8156_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    h->i2c_bus = cfg->i2c_bus;
    h->i2c_address = cfg->i2c_address;

    // Add device to I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_address,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(cfg->i2c_bus, &dev_cfg, &h->dev_handle);
    if (ret != ESP_OK) {
        free(h);
        ESP_LOGE(TAG, "Failed to add I2C device");
        return ret;
    }

    *out_handle = h;
    ESP_LOGI(TAG, "Initialized at I2C addr 0x%02X", cfg->i2c_address);
    return ESP_OK;
}

esp_err_t es8156_deinitialize(es8156_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_rm_device(handle->dev_handle);
    free(handle);
    return ESP_OK;
}

/* ========================================================================
 * Chip ID
 * ======================================================================== */

esp_err_t es8156_read_chip_id(es8156_handle_t h, uint16_t *out_id)
{
    uint8_t id_hi = 0, id_lo = 0;
    ESP_RETURN_ON_ERROR(es8156_read_reg(h, ES8156_REG_CHIP_ID1, &id_hi), TAG, "read ID1");
    ESP_RETURN_ON_ERROR(es8156_read_reg(h, ES8156_REG_CHIP_ID0, &id_lo), TAG, "read ID0");
    *out_id = ((uint16_t)id_hi << 8) | id_lo;
    return ESP_OK;
}

/* ========================================================================
 * ES8156_Reset() — from 8051 ref
 * ======================================================================== */

esp_err_t es8156_reset(es8156_handle_t h)
{
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x00, 0x1C), TAG, "reset(1)");
    return es8156_write_reg(h, 0x00, 0x01);  // Slave Mode
}

/* ========================================================================
 * ES8156_DAC() — from 8051 ref, DACHPModeOn=1 (headphone)
 *
 * Registers are written exactly as in the original ES8156_DAC() function.
 * Mode: DACHPModeOn=1, Ratio=256, VDDA=3.3V, Slave, I2S 16-bit
 * ======================================================================== */

esp_err_t es8156_configure(es8156_handle_t h)
{
    ESP_LOGI(TAG, "Configure DAC (headphone mode, main2 ref DACHPModeOn=1)");

    /*
     * 0x02 = (MCLK_SOURCE<<7) + (SCLK_INV<<4) + (EQ7bandOn<<3) + 0x04 + MSMode_MasterSelOn
     *      = 0 + 0 + 0 + 0x04 + 0 = 0x04   → SOFT_MODE_SEL=1, Slave
     */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x02, 0x04), TAG, "reg 0x02");

    /* 0x13 — disable automute features */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x13, 0x00), TAG, "reg 0x13");

    /* Power-up timing */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x0A, 0x01), TAG, "reg 0x0A");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x0B, 0x01), TAG, "reg 0x0B");

    /* ADF i2s_stream outputs 16-bit Philips I2S.  Format_Len=0 selects a
     * 16-bit serial word; 0x30 selects 32-bit words and amplifies alignment
     * noise on the HT6872 speaker path. */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x11, 0x00), TAG, "reg 0x11");

    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x12, 0x00), TAG, "reg 0x12");

    /* 0x14 = DAC_Volume = 191 (0xBF) = 0dB */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x14, 0xBF), TAG, "reg 0x14");

    /*
     * Ratio == 256:
     *   0x01 = 0x21 + (0x40 * EQ7bandOn) = 0x21
     *   0x09 = 0x00
     */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x01, 0x21), TAG, "reg 0x01");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x09, 0x00), TAG, "reg 0x09");

    /* 0x03/0x04 = LRCK Divider = Ratio = 256 */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x03, 0x01), TAG, "reg 0x03");  // 256 >> 8
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x04, 0x00), TAG, "reg 0x04");  // 256 & 0xFF

    /* 0x05 = SCLK_DIV = 4 */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x05, 0x04), TAG, "reg 0x05");

    /* Remaining init sequence */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x0D, 0x14), TAG, "reg 0x0D");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x18, 0x00), TAG, "reg 0x18");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x08, 0x3F), TAG, "reg 0x08");  // all clocks ON

    /* CSM sequence: 0x02 → 0x03 */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x00, 0x02), TAG, "reg 0x00(1)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x00, 0x03), TAG, "reg 0x00(2)");

    /* 0x25 = 0x20 — power up analog */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x25, 0x20), TAG, "reg 0x25");

    /*
     * DACHPModeOn == 1 — 耳机驱动 (headphone driver)
     */
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x20, 0x16), TAG, "reg 0x20");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x21, 0x3F), TAG, "reg 0x21");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x22, 0x0A), TAG, "reg 0x22");  // HPSW=1, SWRMPSEL=1
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x24, 0x01), TAG, "reg 0x24");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x23, 0xCA), TAG, "reg 0x23");  // VDDA=3.3V

    ESP_LOGI(TAG, "DAC configured (headphone mode)");
    return ESP_OK;
}

/* ========================================================================
 * ES8156_Powerdown() — from 8051 ref
 * ======================================================================== */

esp_err_t es8156_powerdown(es8156_handle_t h)
{
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x14, 0x00), TAG, "pd(0x14)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x19, 0x02), TAG, "pd(0x19)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x22, 0x02), TAG, "pd(0x22)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x25, 0x81), TAG, "pd(0x25)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x18, 0x01), TAG, "pd(0x18)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x09, 0x02), TAG, "pd(0x09.1)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x09, 0x01), TAG, "pd(0x09.2)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x08, 0x00), TAG, "pd(0x08)");
    vTaskDelay(pdMS_TO_TICKS(500));  // DELAY_MS(500)
    return es8156_write_reg(h, 0x25, 0x87);
}

/* ========================================================================
 * ES8156_Standby_NoPop() — from 8051 ref
 * ======================================================================== */

esp_err_t es8156_standby_nopop(es8156_handle_t h)
{
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x14, 0x00), TAG, "stby(0x14)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x19, 0x02), TAG, "stby(0x19)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x25, 0xA1), TAG, "stby(0x25)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x18, 0x01), TAG, "stby(0x18)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x09, 0x02), TAG, "stby(0x09.1)");
    ESP_RETURN_ON_ERROR(es8156_write_reg(h, 0x09, 0x01), TAG, "stby(0x09.2)");
    return es8156_write_reg(h, 0x08, 0x00);
}
