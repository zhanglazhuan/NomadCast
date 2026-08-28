// SPDX-License-Identifier: MIT
// Ported from Everest ES8156 8051 reference code (ES8156.C) to ESP-IDF
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Config & Handle ---- */
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_address;    // 7-bit I2C address (ES8156 default: 0x08)
} es8156_config_t;

typedef struct es8156 *es8156_handle_t;

/* ---- Init / Deinit ---- */
esp_err_t es8156_initialize(const es8156_config_t *cfg, es8156_handle_t *out_handle);
esp_err_t es8156_deinitialize(es8156_handle_t handle);

/* ---- Low-level register access ---- */
esp_err_t es8156_write_reg(es8156_handle_t handle, uint8_t reg, uint8_t value);
esp_err_t es8156_read_reg(es8156_handle_t handle, uint8_t reg, uint8_t *value);

/* ---- Chip ID ---- */
esp_err_t es8156_read_chip_id(es8156_handle_t handle, uint16_t *out_id);

/* ---- High-level sequences (ported from ES8156.C) ---- */

/**
 * @brief Full DAC init (equivalent to ES8156_DAC() in 8051 ref)
 *
 * Uses DACHPModeOn=1 (headphone driver mode).
 * Ratio=256, Format=I2S, Len=16bit, SCLK_DIV=4, VDDA=3.3V, Slave mode.
 */
esp_err_t es8156_configure(es8156_handle_t handle);

/** @brief Power-down sequence (ES8156_Powerdown) */
esp_err_t es8156_powerdown(es8156_handle_t handle);

/** @brief Standby without pop (ES8156_Standby_NoPop) */
esp_err_t es8156_standby_nopop(es8156_handle_t handle);

/** @brief Reset sequence (ES8156_Reset) */
esp_err_t es8156_reset(es8156_handle_t handle);

#ifdef __cplusplus
}
#endif
