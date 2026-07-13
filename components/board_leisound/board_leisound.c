/*
 * Leisound V1 Board Hardware Abstraction — Implementation
 *
 * All init functions are self-contained and return esp_err_t.
 * Errors are logged with ESP_LOGE before returning.
 */

#include "board_leisound.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

/* ========================================================================
 * Power
 * ======================================================================== */

esp_err_t board_power_init(void)
{
    ESP_LOGI(TAG, "Power ON: EN_POWER(46)=1, AP_EN(21)=1");

    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(BOARD_PIN_EN_POWER) | BIT64(BOARD_PIN_AP_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio_config power pins");

    gpio_set_level(BOARD_PIN_EN_POWER, 1);
    gpio_set_level(BOARD_PIN_AP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));  // wait for power stabilization

    // AMP_DET: input with pulldown
    gpio_config_t det_cfg = {
        .pin_bit_mask = BIT64(BOARD_PIN_AMP_DET),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&det_cfg);

    ESP_LOGI(TAG, "Power OK, AMP_DET=%d", gpio_get_level(BOARD_PIN_AMP_DET));
    return ESP_OK;
}

/* ========================================================================
 * I2C Bus
 * ======================================================================== */

esp_err_t board_i2c_init(i2c_master_bus_handle_t *out_bus)
{
    ESP_RETURN_ON_FALSE(out_bus, ESP_ERR_INVALID_ARG, TAG, "out_bus is NULL");

    ESP_LOGI(TAG, "I2C0 init: SDA=%d SCL=%d @ %d Hz",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_I2C_FREQ_HZ);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port    = I2C_NUM_0,
        .sda_io_num  = BOARD_PIN_I2C_SDA,
        .scl_io_num  = BOARD_PIN_I2C_SCL,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, out_bus),
                        TAG, "i2c_new_master_bus");

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "I2C0 bus ready");
    return ESP_OK;
}

/* ========================================================================
 * ES8156 DAC
 * ======================================================================== */

esp_err_t board_es8156_init(i2c_master_bus_handle_t i2c_bus,
                             es8156_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(i2c_bus && out_handle, ESP_ERR_INVALID_ARG,
                        TAG, "NULL arg");

    ESP_LOGI(TAG, "ES8156 init at addr 0x%02X (headphone mode)",
             BOARD_ES8156_I2C_ADDR);

    es8156_config_t cfg = {
        .i2c_bus     = i2c_bus,
        .i2c_address = BOARD_ES8156_I2C_ADDR,
    };

    ESP_RETURN_ON_ERROR(es8156_initialize(&cfg, out_handle),
                        TAG, "es8156_initialize");

    // Read and log chip ID
    uint16_t chip_id = 0;
    if (es8156_read_chip_id(*out_handle, &chip_id) == ESP_OK) {
        ESP_LOGI(TAG, "ES8156 Chip ID: 0x%04X", chip_id);
    }

    // Full DAC configure (headphone mode, from 8051 reference code)
    ESP_RETURN_ON_ERROR(es8156_configure(*out_handle),
                        TAG, "es8156_configure");

    ESP_LOGI(TAG, "ES8156 ready (headphone mode)");
    return ESP_OK;
}

/* ========================================================================
 * SD Card
 * ======================================================================== */

esp_err_t board_sd_init(void)
{
    ESP_LOGI(TAG, "SD card init: CLK=%d CMD=%d D0=%d (1-bit mode)",
             BOARD_PIN_SD_CLK, BOARD_PIN_SD_CMD, BOARD_PIN_SD_D0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = BOARD_PIN_SD_CLK;
    slot.cmd   = BOARD_PIN_SD_CMD;
    slot.d0    = BOARD_PIN_SD_D0;
    slot.width = 1;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT,
                                             &host, &slot,
                                             &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD mounted at %s", BOARD_SD_MOUNT_POINT);
    return ESP_OK;
}

/* ========================================================================
 * Headphone detect
 * ======================================================================== */

esp_err_t board_headphone_detected(bool *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    *out = (gpio_get_level(BOARD_PIN_AMP_DET) == 1);
    return ESP_OK;
}
