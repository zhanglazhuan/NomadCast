#include "nomadcast.h"
/*
 * PCB V1.2 — Display + Touch Board HAL
 *
 * Pin map for ST7789V (SPI2) + GT911 (I2C0) on PCB V1.2.
 *
 * Init order (matches t_display_touch exactly):
 *   1. board_ui_power_on()
 *   2. board_ui_hw_reset()        — combined LCD+Touch reset on shared GPIO43
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
 * Pin Map (PCB V1.2 — ESP32-S3)
 * ======================================================================== */

/* Power */
#define BOARD_UI_PIN_EN_POWER NOMADCAST_PIN_AP_POWER

/* SPI2 — LCD (ST7789V) */
#define BOARD_UI_PIN_SPI_SCK NOMADCAST_PIN_SPI_SCK
#define BOARD_UI_PIN_SPI_MOSI NOMADCAST_PIN_SPI_MOSI
#define BOARD_UI_PIN_SPI_MISO NOMADCAST_PIN_SPI_MISO
#define BOARD_UI_PIN_LCD_CS NOMADCAST_PIN_LCD_CS
#define BOARD_UI_PIN_LCD_DC NOMADCAST_PIN_LCD_DC
#define BOARD_UI_PIN_LCD_RST NOMADCAST_PIN_LCD_RST

/* I2C0 — Touch (GT911) */
#define BOARD_UI_PIN_I2C_SDA NOMADCAST_PIN_I2C_SDA
#define BOARD_UI_PIN_I2C_SCL NOMADCAST_PIN_I2C_SCL
#define BOARD_UI_PIN_TP_INT NOMADCAST_PIN_TP_INT
#define BOARD_UI_PIN_TP_RST NOMADCAST_PIN_TP_RST

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
 * @brief Combined hardware reset for LCD and Touch (shared GPIO43).
 *
 * Matches t_display_touch combined_hw_reset() exactly:
 *   INT=0, RST=0 → 200ms → RST=1 → 200ms → INT=1 (input+pullup) → 200ms
 */
esp_err_t board_ui_hw_reset(void);

/**
 * @brief Initialize display (ST7789V via SPI2 @ 20MHz).
 *
 * Call AFTER board_ui_hw_reset(). Uses reset_gpio_num=GPIO_NUM_NC
 * since the shared GPIO43 reset was already done.
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
