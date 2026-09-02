/*
 * GT911 Capacitive Touch Controller Driver
 *
 * Ported from H7_GT911_I2C_Touch-master (STM32H7 working driver) to ESP-IDF.
 *
 * Key facts:
 *   - I2C address = 0xBA (7-bit = 0x5D), fallback 0x14
 *   - Reset sequence: RST+INT LOW 200ms → RST HI 200ms → INT HI 200ms
 *   - Factory config works out of the box; no config write needed
 *   - Scan: read status → if buf_rdy → read touch count (1-5) → read points → clear status
 *   - ALWAYS write 0 to status reg after reading
 */

#ifndef GT911_H
#define GT911_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---- */

#define GT911_MAX_TOUCH_POINTS  5
#define GT911_MAX_WIDTH         800
#define GT911_MAX_HEIGHT        480

/* ---- Register addresses ---- */

#define GT911_PRODUCT_ID_REG        0x8140
#define GT911_CONFIG_REG            0x8047
#define GT911_FIRMWARE_VERSION_REG  0x8144
#define GT911_READ_XY_REG           0x814E

/* ---- I2C addresses ---- */

#define GT911_I2C_ADDR              0xBA   /* 7-bit = 0x5D */
#define GT911_I2C_ADDR_ALT          0x14   /* Fallback address */

/* ---- Types ---- */

/**
 * @brief Touch point data for a single contact
 */
typedef struct {
    uint8_t  track_id;      /**< Touch tracking ID (0~5) */
    uint16_t x;             /**< X coordinate (0~GT911_MAX_WIDTH) */
    uint16_t y;             /**< Y coordinate (0~GT911_MAX_HEIGHT) */
    uint16_t size;          /**< Touch area size */
} gt911_touch_point_t;

/**
 * @brief Full touch scan result
 */
typedef struct {
    uint8_t             count;                              /**< Number of touch points (0~5) */
    gt911_touch_point_t points[GT911_MAX_TOUCH_POINTS];     /**< Touch point data */
} gt911_touch_data_t;

/**
 * @brief Touch event callback type.
 * @param points    Touch point data.
 * @param count     Number of touch points (0 = release).
 * @param user_data Opaque user data passed to gt911_register_isr().
 */
typedef void (*gt911_touch_cb_t)(const gt911_touch_point_t *points,
                                 uint8_t count, void *user_data);

/**
 * @brief Hardware configuration for GT911
 */
typedef struct {
    gpio_num_t  rst_pin;        /**< Reset pin (output) */
    gpio_num_t  int_pin;        /**< Interrupt pin (output during reset, then input) */
    gpio_num_t  i2c_sda_pin;    /**< I2C SDA pin */
    gpio_num_t  i2c_scl_pin;    /**< I2C SCL pin */
    uint32_t    i2c_freq_hz;    /**< I2C clock frequency (default 100000) */
    uint16_t    max_width;      /**< Expected display width for coord clamping */
    uint16_t    max_height;     /**< Expected display height for coord clamping */
    bool        use_interrupt;  /**< Enable interrupt-driven mode (INT pin falling edge) */
} gt911_config_t;

/**
 * @brief GT911 device handle (opaque)
 */
typedef struct gt911_dev_t gt911_dev_t;

/* ---- API ---- */

/**
 * @brief Initialize GT911 touch controller.
 *
 * Powers up the chip, performs the hardware reset sequence, initializes I2C,
 * probes the device, and verifies product ID.
 *
 * @param[in]  config   Hardware pin and bus configuration.
 * @param[out] out_dev  Pointer to receive the device handle.
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_NOT_FOUND if GT911 not detected on I2C bus
 *   - ESP_ERR_INVALID_ARG if config or out_dev is NULL
 *   - ESP_FAIL on I2C or GPIO error
 */
esp_err_t gt911_init(const gt911_config_t *config, gt911_dev_t **out_dev);

/**
 * @brief Perform a single touch scan (polling mode).
 *
 * Reads the status register; if touch data is ready, reads all active
 * touch points and clears the status register.
 *
 * @param[in]  dev   Device handle from gt911_init().
 * @param[out] data  Pointer to receive touch data (may be NULL to discard).
 * @return
 *   - ESP_OK on success (data->count == 0 means no touch)
 *   - ESP_ERR_INVALID_ARG if dev is NULL
 */
esp_err_t gt911_scan(gt911_dev_t *dev, gt911_touch_data_t *data);

/**
 * @brief Read the firmware version register.
 *
 * @param[in]  dev      Device handle.
 * @param[out] version  Pointer to receive firmware version.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t gt911_get_firmware_version(gt911_dev_t *dev, uint16_t *version);

/**
 * @brief Deinitialize the GT911 driver and free resources.
 *
 * @param[in] dev  Device handle to free (may be NULL).
 * @return ESP_OK on success.
 */
esp_err_t gt911_deinit(gt911_dev_t *dev);

/**
 * @brief Register for interrupt-driven touch events.
 *
 * Configures the INT pin as falling-edge interrupt input. When a touch
 * event is detected, the ISR wakes a FreeRTOS task which performs the
 * I2C scan and calls `cb` from task context.
 *
 * Only valid when use_interrupt=true in gt911_config_t.
 *
 * @param dev        Device handle.
 * @param cb         Callback invoked from task context on touch events.
 * @param user_data  Passed through to cb.
 * @return ESP_OK on success.
 */
esp_err_t gt911_register_isr(gt911_dev_t *dev, gt911_touch_cb_t cb, void *user_data);

/**
 * @brief Suspend touch sensing — disable INT GPIO interrupt.
 *
 * After suspend, touch events stop. GT911 stays powered but INT is ignored.
 * Call gt911_resume() to re-enable.
 *
 * @param dev  Device handle.
 * @return ESP_OK on success.
 */
esp_err_t gt911_suspend(gt911_dev_t *dev);

/**
 * @brief Resume touch sensing — re-enable INT GPIO interrupt.
 *
 * @param dev  Device handle.
 * @return ESP_OK on success.
 */
esp_err_t gt911_resume(gt911_dev_t *dev);

/**
 * @brief Quick check: is a touch currently active?
 *
 * Reads the status register in polling mode (no ISR needed).
 * Useful for health checks without enabling the full interrupt pipeline.
 *
 * @param dev  Device handle.
 * @return true if touch data is ready in the GT911 buffer.
 */
bool gt911_is_touched(gt911_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* GT911_H */
