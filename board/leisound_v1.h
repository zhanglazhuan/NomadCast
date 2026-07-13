/*
 * Leisound V1 Board Pin Definitions
 *
 * Central hardware configuration for the ESP32-S3 Leisound V1 board.
 * All component code references pins from this header — single source of truth.
 */

#ifndef LEISOUND_V1_H
#define LEISOUND_V1_H

#include "driver/gpio.h"
#include "hal/spi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Power
 * ======================================================================== */

#define LEISOUND_PIN_EN_POWER    GPIO_NUM_46   /* EN_PWR: board peripherals power enable (HIGH=ON) */

/* ========================================================================
 * SPI2 — ST7789V LCD (240x320, RGB565, 4-Wire SPI)
 * ======================================================================== */

#define LEISOUND_LCD_HOST        SPI2_HOST     /* SPI2_HOST = 2 */
#define LEISOUND_LCD_H_RES       240
#define LEISOUND_LCD_V_RES       320
#define LEISOUND_LCD_BIT_PER_PIXEL  16

#define LEISOUND_PIN_LCD_CS      GPIO_NUM_10   /* Chip select (LOW active) */
#define LEISOUND_PIN_LCD_DC      GPIO_NUM_45   /* Data/Command (HIGH=data, LOW=cmd) */
#define LEISOUND_PIN_LCD_RST     GPIO_NUM_8    /* Hardware reset (LOW active) */
#define LEISOUND_PIN_SPI_SCK     GPIO_NUM_12   /* SPI2 clock */
#define LEISOUND_PIN_SPI_MOSI    GPIO_NUM_11   /* SPI2 MOSI */
#define LEISOUND_PIN_SPI_MISO    GPIO_NUM_13   /* SPI2 MISO (unused by LCD) */

/* ========================================================================
 * I2C0 — GT911 Touch Controller
 * ======================================================================== */

#define LEISOUND_TOUCH_I2C_PORT  I2C_NUM_0     /* I2C port 0 */
#define LEISOUND_PIN_I2C_SDA     GPIO_NUM_47   /* I2C0 data */
#define LEISOUND_PIN_I2C_SCL     GPIO_NUM_48   /* I2C0 clock */
#define LEISOUND_PIN_TP_INT      GPIO_NUM_18   /* Touch interrupt (active high) */
#define LEISOUND_PIN_TP_RST      GPIO_NUM_8    /* Touch reset (shared with LCD RST) */

#define LEISOUND_GT911_ADDR_1    0x5D          /* Primary I2C address */
#define LEISOUND_GT911_ADDR_2    0x14          /* Fallback I2C address */

/* ========================================================================
 * LVGL v9 Settings
 * ======================================================================== */

#define LEISOUND_LVGL_BUF_HEIGHT       30
#define LEISOUND_LVGL_TICK_PERIOD_MS   2
#define LEISOUND_LVGL_TASK_MAX_DELAY   500
#define LEISOUND_LVGL_TASK_MIN_DELAY   5
#define LEISOUND_LVGL_TASK_STACK       (8 * 1024)
#define LEISOUND_LVGL_TASK_PRIORITY    2

/* ========================================================================
 * Audio I2S (placeholder — verify against schematic)
 * ======================================================================== */

#define LEISOUND_AUDIO_I2S_PORT    I2S_NUM_0
#define LEISOUND_PIN_I2S_MCLK      GPIO_NUM_4    /* No MCLK for HT6872 direct I2S */
#define LEISOUND_PIN_I2S_BCLK      GPIO_NUM_5    /* I2S bit clock */
#define LEISOUND_PIN_I2S_WS        GPIO_NUM_6   /* I2S word select / LRCLK */
#define LEISOUND_PIN_I2S_DOUT      GPIO_NUM_NC   /* I2S data out (to amp) */
#define LEISOUND_PIN_I2S_DIN       GPIO_NUM_7   /* I2S data in (from mic — placeholder) */
#define LEISOUND_PIN_AMP_EN        GPIO_NUM_43   /* Amp enable (shared with EN_POWER) */

/* ========================================================================
 * Battery ADC (placeholder — verify against schematic)
 * ======================================================================== */

#define LEISOUND_BAT_ADC_UNIT      ADC_UNIT_1
#define LEISOUND_BAT_ADC_CHANNEL   ADC_CHANNEL_3 /* Placeholder — update from schematic */
#define LEISOUND_BAT_EN_PIN        GPIO_NUM_NC   /* Battery measurement enable */
#define LEISOUND_BAT_KEY_PIN       GPIO_NUM_NC   /* Power key status */
#define LEISOUND_BAT_CHG_PIN       GPIO_NUM_NC   /* Charging status */

/* ========================================================================
 * Physical Keys (GPIO buttons)
 * ======================================================================== */

#define LEISOUND_PIN_KEY_VOL_DOWN   GPIO_NUM_42   /* Vol- : short=vol-, long=prev */
#define LEISOUND_PIN_KEY_VOL_UP     GPIO_NUM_41   /* Vol+ : short=vol+, long=next */
#define LEISOUND_PIN_KEY_STOP       GPIO_NUM_40   /* Stop : short=play/pause */
#define LEISOUND_PIN_KEY_POWER      GPIO_NUM_39   /* PWR  : short=screen, long=shutdown */

#ifdef __cplusplus
}
#endif

#endif /* LEISOUND_V1_H */
