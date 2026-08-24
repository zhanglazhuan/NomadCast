/*
 * NomadCast V1 Board Pin Definitions
 *
 * Central hardware configuration for the NomadCast V1 board (ESP32-S3).
 * All component code references pins from this header — single source of truth.
 *
 * Consolidated from the debug validation projects (t_display, t_touch_bitbanging,
 * t_battery, t_key, t_sd, t_speaker, t_earphone2), which were debugged against
 * the V1.1 schematic.
 *
 * NOTE: Touch (GT911) and codec (ES8156) share a software bit-banged I2C bus
 * (SDA=38 / SCL=45). SCL on GPIO45 is a VDD_SPI strapping pin whose pull-down
 * breaks the hardware I2C driver — see drivers/sw_i2c.
 */

#ifndef NOMADCAST_V1_H
#define NOMADCAST_V1_H

#include "driver/gpio.h"
#include "hal/spi_types.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Power
 * ======================================================================== */

#define NOMADCAST_PIN_AP_POWER    GPIO_NUM_46   /* 全板外设电源使能 (HIGH=ON) */
#define NOMADCAST_PIN_LCD_POWER   GPIO_NUM_43   /* 屏幕电源 (HIGH=ON) — 独立于 AP power */

/* ========================================================================
 * SPI2 — ST7789V LCD (240x320, RGB565, 4-Wire SPI)
 * ======================================================================== */

#define NOMADCAST_LCD_HOST           SPI2_HOST
#define NOMADCAST_LCD_H_RES          240
#define NOMADCAST_LCD_V_RES          320
#define NOMADCAST_LCD_BIT_PER_PIXEL  16

#define NOMADCAST_PIN_LCD_CS      GPIO_NUM_48   /* 片选 (LOW active) */
#define NOMADCAST_PIN_LCD_DC      GPIO_NUM_47   /* 数据/命令 (HIGH=data) */
#define NOMADCAST_PIN_LCD_RST     GPIO_NUM_40   /* 硬件复位 (LOW active) — 与 TP_RST 共用 */
#define NOMADCAST_PIN_LCD_BL      GPIO_NUM_12   /* 背光 (HIGH=ON) */
#define NOMADCAST_PIN_LCD_INT     GPIO_NUM_39   /* 屏幕中断 — 与 TP_INT 共用 */
#define NOMADCAST_PIN_SPI_SCK     GPIO_NUM_21   /* SPI2 clock */
#define NOMADCAST_PIN_SPI_MOSI    GPIO_NUM_14   /* SPI2 MOSI */
#define NOMADCAST_PIN_SPI_MISO    GPIO_NUM_13   /* SPI2 MISO (LCD 未用) */

/* ========================================================================
 * I2C (software bit-bang) — GT911 touch + ES8156 codec 共享
 * ======================================================================== */

#define NOMADCAST_PIN_I2C_SDA     GPIO_NUM_38   /* 软件 I2C SDA */
#define NOMADCAST_PIN_I2C_SCL     GPIO_NUM_45   /* 软件 I2C SCL (strapping 引脚) */
#define NOMADCAST_PIN_TP_INT      GPIO_NUM_39   /* Touch 中断 (active high) */
#define NOMADCAST_PIN_TP_RST      GPIO_NUM_40   /* Touch 复位 — 与 LCD_RST 共用 */

#define NOMADCAST_GT911_ADDR_1    0x5D          /* GT911 主 I2C 地址 (7-bit) */
#define NOMADCAST_GT911_ADDR_2    0x14          /* GT911 备用 I2C 地址 (7-bit) */
#define NOMADCAST_ES8156_ADDR     0x08          /* ES8156 DAC I2C 地址 (7-bit) */

/* ========================================================================
 * LVGL v9 Settings
 * ======================================================================== */

#define NOMADCAST_LVGL_BUF_HEIGHT       30
#define NOMADCAST_LVGL_TICK_PERIOD_MS   2
#define NOMADCAST_LVGL_TASK_MAX_DELAY   500
#define NOMADCAST_LVGL_TASK_MIN_DELAY   5
#define NOMADCAST_LVGL_TASK_STACK       (8 * 1024)
#define NOMADCAST_LVGL_TASK_PRIORITY    2

/* ========================================================================
 * Audio — ES8156 DAC + HT6872 功放 (I2S)
 * ======================================================================== */

#define NOMADCAST_PIN_I2S_MCLK    GPIO_NUM_1    /* I2S 主时钟 (ESP32 输出给 ES8156) */
#define NOMADCAST_PIN_I2S_BCLK    GPIO_NUM_2    /* I2S 位时钟 */
#define NOMADCAST_PIN_I2S_WS      GPIO_NUM_41   /* I2S 字选择 / LRCLK */
#define NOMADCAST_PIN_I2S_DOUT    GPIO_NUM_42   /* I2S 数据输出 (到 DAC/功放) */
#define NOMADCAST_PIN_I2S_DIN     GPIO_NUM_NC   /* I2S 数据输入 (无 mic) */

#define NOMADCAST_PIN_AMP_EN      GPIO_NUM_44   /* HT6872 功放使能 (HIGH=喇叭开) */
#define NOMADCAST_PIN_HP_DETECT   GPIO_NUM_18   /* 耳机检测 (输入, HIGH=已插入) */

/* ========================================================================
 * Battery (ADC + 充电状态)
 * ======================================================================== */

#define NOMADCAST_BAT_ADC_UNIT      ADC_UNIT_1
#define NOMADCAST_BAT_ADC_CHANNEL   ADC_CHANNEL_7 /* GPIO8 — 电池电压采样 */
#define NOMADCAST_BAT_CHG_PIN       GPIO_NUM_4    /* 充电状态 (LOW=充电中) */
#define NOMADCAST_BAT_DIVIDER       2             /* 板上分压电阻倍率 */

/* ========================================================================
 * SD Card (1-bit SDMMC)
 * ======================================================================== */

#define NOMADCAST_PIN_SD_CLK       GPIO_NUM_10   /* SD CLK */
#define NOMADCAST_PIN_SD_CMD       GPIO_NUM_11   /* SD CMD */
#define NOMADCAST_PIN_SD_DAT0      GPIO_NUM_9    /* SD DAT0 */

/* ========================================================================
 * Physical Keys (GPIO buttons)
 * ======================================================================== */

#define NOMADCAST_PIN_KEY_VOL_DOWN   GPIO_NUM_6    /* Vol- : short=vol-, long=prev */
#define NOMADCAST_PIN_KEY_VOL_UP     GPIO_NUM_17   /* Vol+ : short=vol+, long=next */
#define NOMADCAST_PIN_KEY_STOP       GPIO_NUM_7    /* Stop : short=play/pause */
#define NOMADCAST_PIN_KEY_POWER      GPIO_NUM_5    /* Power: short=screen toggle, long=power off */

#ifdef __cplusplus
}
#endif

#endif /* NOMADCAST_V1_H */
