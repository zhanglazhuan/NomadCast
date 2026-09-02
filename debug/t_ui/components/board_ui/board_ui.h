/*
 * Leisound V1 — Display + Touch Board HAL
 *
 * Pin map for ST7789V (SPI2) + GT911 (I2C0) on Leisound V1.
 *
 * Init order (matches t_display_touch exactly):
 *   1. board_ui_power_on()
 *   2. board_ui_hw_reset()        — combined LCD+Touch reset on shared GPIO8
 *   3. board_ui_display_init()    — ST7789 via SPI2 (reset_gpio_num=NC)
 *   4. board_ui_touch_init()      — GT911 via I2C0 (no reset, already done)
 */

#ifndef BOARD_UI_H
#define BOARD_UI_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Pin Map (Leisound V1 — ESP32-S3)
 * ======================================================================== */

/* Power */
#define BOARD_UI_PIN_EN_POWER    GPIO_NUM_46

/* SPI2 — LCD (ST7789V) */
#define BOARD_UI_PIN_SPI_SCK     GPIO_NUM_12
#define BOARD_UI_PIN_SPI_MOSI    GPIO_NUM_11
#define BOARD_UI_PIN_SPI_MISO    GPIO_NUM_13
#define BOARD_UI_PIN_LCD_CS      GPIO_NUM_10
#define BOARD_UI_PIN_LCD_DC      GPIO_NUM_45
#define BOARD_UI_PIN_LCD_RST     GPIO_NUM_8    /* Shared with TP RST */

/* I2C0 — Touch (GT911) */
#define BOARD_UI_PIN_I2C_SDA     GPIO_NUM_47
#define BOARD_UI_PIN_I2C_SCL     GPIO_NUM_48
#define BOARD_UI_PIN_TP_INT      GPIO_NUM_18
#define BOARD_UI_PIN_TP_RST      GPIO_NUM_8    /* Shared with LCD RST */

/* Display */
#define BOARD_UI_LCD_W           240
#define BOARD_UI_LCD_H           320

/* ========================================================================
 * Touch types (lightweight, mirrors drivers/gt911 for scan compatibility)
 * ======================================================================== */

#define BOARD_UI_TOUCH_MAX_POINTS 5

typedef struct {
    uint8_t  track_id;
    uint16_t x;
    uint16_t y;
    uint16_t size;
} board_ui_touch_point_t;

typedef struct {
    uint8_t                count;
    board_ui_touch_point_t points[BOARD_UI_TOUCH_MAX_POINTS];
} board_ui_touch_data_t;

/* Opaque touch device handle */
typedef struct board_ui_touch_dev_t board_ui_touch_dev_t;

/* ========================================================================
 * Public API
 * ======================================================================== */

/** @brief Enable board peripheral power (EN_POWER=HIGH). */
esp_err_t board_ui_power_on(void);

/**
 * @brief Combined hardware reset for LCD and Touch (shared GPIO8).
 *
 * Matches t_display_touch combined_hw_reset() exactly:
 *   INT=0, RST=0 → 200ms → RST=1 → 200ms → INT=1 (input+pullup) → 200ms
 */
esp_err_t board_ui_hw_reset(void);

/**
 * @brief Initialize display (ST7789V via SPI2 @ 20MHz).
 *
 * Call AFTER board_ui_hw_reset(). Uses reset_gpio_num=GPIO_NUM_NC
 * since the shared GPIO8 reset was already done.
 *
 * @param[out] panel  Receives the esp_lcd panel handle.
 */
esp_err_t board_ui_display_init(esp_lcd_panel_handle_t *panel);

/**
 * @brief Initialize touch (GT911 via I2C0 @ 100kHz).
 *
 * Call AFTER board_ui_hw_reset() and board_ui_display_init().
 * Does NOT perform hardware reset — assumes board_ui_hw_reset()
 * was already called.
 *
 * @param[out] dev  Receives the touch device handle.
 */
esp_err_t board_ui_touch_init(board_ui_touch_dev_t **dev);

/**
 * @brief Scan touch points (polling mode).
 *
 * @param[in]  dev   Touch device handle.
 * @param[out] data  Touch data output. data->count == 0 means no touch.
 * @return ESP_OK on success.
 */
esp_err_t board_ui_touch_scan(board_ui_touch_dev_t *dev,
                              board_ui_touch_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_UI_H */
