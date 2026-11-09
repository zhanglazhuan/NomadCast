#include "nomadcast.h"
// SPDX-License-Identifier: MIT
// Leisound V1 Board Hardware Abstraction — Pin Map & Init API
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
#include "es8156.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Pin Map (Leisound V1 — ESP32-S3)
 * ======================================================================== */

/* ---- Power ---- */
#define BOARD_PIN_EN_POWER NOMADCAST_PIN_AP_POWER
#define BOARD_PIN_AP_EN NOMADCAST_PIN_AMP_EN
#define BOARD_PIN_AMP_DET NOMADCAST_PIN_HP_DETECT

/* ---- I2C (共用总线) ---- */
#define BOARD_PIN_I2C_SDA NOMADCAST_PIN_I2C_SDA
#define BOARD_PIN_I2C_SCL NOMADCAST_PIN_I2C_SCL
#define BOARD_I2C_FREQ_HZ    400000

/* ---- I2S ---- */
#define BOARD_PIN_I2S_BCLK NOMADCAST_PIN_I2S_BCLK
#define BOARD_PIN_I2S_LRCLK NOMADCAST_PIN_I2S_WS
#define BOARD_PIN_I2S_DOUT NOMADCAST_PIN_I2S_DOUT
#define BOARD_I2S_PORT       I2S_NUM_0

/* ---- SDMMC (1-bit mode) ---- */
#define BOARD_PIN_SD_CLK NOMADCAST_PIN_SD_CLK
#define BOARD_PIN_SD_CMD NOMADCAST_PIN_SD_CMD
#define BOARD_PIN_SD_D0 NOMADCAST_PIN_SD_DAT0
#define BOARD_SD_MOUNT_POINT "/sdcard"

/* ---- ES8156 ---- */
#define BOARD_ES8156_I2C_ADDR NOMADCAST_ES8156_ADDR

/* ========================================================================
 * Public API
 * ======================================================================== */

/** @brief Enable board peripheral power (EN_POWER=1, AP_EN=1) */
esp_err_t board_power_init(void);

/**
 * @brief Initialize shared I2C0 bus.
 * @param[out] out_bus  Receives the I2C master bus handle.
 *                      Can be shared with ES8156, GT911, etc.
 */
esp_err_t board_i2c_init(i2c_master_bus_handle_t *out_bus);

/**
 * @brief Initialize ES8156 DAC in headphone mode on the shared I2C bus.
 * @param[in]  i2c_bus     I2C bus handle from board_i2c_init().
 * @param[out] out_handle  Receives the ES8156 device handle.
 */
esp_err_t board_es8156_init(i2c_master_bus_handle_t i2c_bus,
                             es8156_handle_t *out_handle);

/**
 * @brief Mount SD card via SDMMC 1-bit mode.
 *        Mounts to /sdcard (BOARD_SD_MOUNT_POINT).
 */
esp_err_t board_sd_init(void);

/**
 * @brief Check headphone insertion status.
 * @param[out] out  true = headphone plugged in.
 */
esp_err_t board_headphone_detected(bool *out);

#ifdef __cplusplus
}
#endif
