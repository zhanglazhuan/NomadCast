/*
 * Leisound V1 — Display + Touch Combined Test (ESP-IDF)
 *
 * === Leisound V1 针脚定义 (参考 boards/espressif/leisound_v1 GT911) ===
 *
 *   信号     | GPIO | 说明
 *   ---------|------|----------------------------------
 *   EN_POWER |  46  | 全板外设电源使能 (HIGH=ON)
 *   SPI SCK  |  12  | SPI2 时钟 (LCD)
 *   SPI MOSI |  11  | SPI2 主机输出 (LCD)
 *   SPI MISO |  13  | SPI2 主机输入 (LCD 不需要)
 *   LCD CS   |  10  | 屏幕片选 (LOW 有效)
 *   LCD DC   |  45  | 数据/命令选择
 *   LCD RST  |   8  | LCD 硬件复位 (与触摸共用)
 *   I2C SDA  |  47  | I2C0 数据 (GT911)
 *   I2C SCL  |  48  | I2C0 时钟 (GT911)
 *   TP INT   |  18  | 触摸中断 (GT911, active high)
 *   TP RST   |   8  | 触摸复位 (与 LCD RST 共用)
 *
 * 屏幕: ST7789V, 240x320, RGB565, 4-Wire SPI @ 20MHz
 * 触摸: GT911 (Goodix), I2C addr 0x14 / 0x5D
 *
 * 测试流程:
 *   1. 上电 + 复位 (LCD + Touch 共用 GPIO8)
 *   2. 初始化 SPI 总线 + ST7789V 屏幕
 *   3. 初始化 I2C 总线 + 扫描/识别 GT911
 *   4. 显示 UI 框架 + 实时触摸点可视化
 *   5. 轮询触摸数据, 在屏幕上画点和坐标
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "t_disp_touch";

/* ========================================================================
 * Pin Definitions — Leisound V1
 * ======================================================================== */

/* Power */
#define PIN_LCD_POWER  GPIO_NUM_43
#define PIN_BACKLIGHT  GPIO_NUM_12

#define PIN_LCD_INT    GPIO_NUM_39
#define PIN_LCD_CS     GPIO_NUM_48
#define PIN_LCD_DC     GPIO_NUM_47
#define PIN_LCD_RST    GPIO_NUM_40

/* SPI2 pins */
#define PIN_SPI_SCK    GPIO_NUM_21
#define PIN_SPI_MOSI   GPIO_NUM_14
#define PIN_SPI_MISO   GPIO_NUM_13

/* I2C0 — Touch */
#define PIN_I2C_SDA     GPIO_NUM_38
#define PIN_I2C_SCL     GPIO_NUM_45
#define PIN_TP_INT      GPIO_NUM_39
#define PIN_TP_RST      GPIO_NUM_40    /* 与 LCD RST 共用 */

/* ========================================================================
 * Display Constants
 * ======================================================================== */

#define LCD_W           240
#define LCD_H           320
#define LCD_BUF_SIZE    (LCD_W * LCD_H * sizeof(uint16_t))  /* 153600 bytes */

#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (20 * 1000 * 1000)

/* ========================================================================
 * Touch Constants
 * ======================================================================== */

#define I2C_MASTER_FREQ_HZ      (100 * 1000)
#define I2C_MASTER_TIMEOUT_MS   100

#define GT911_ADDR_1    0x5D   /* primary (H7 driver: 0xBA >> 1 = 0x5D) */
#define GT911_ADDR_2    0x14   /* fallback */

/* GT911 Registers */
#define GT911_REG_PRODUCT_ID        0x8140
#define GT911_REG_FIRMWARE_VERSION  0x8144
#define GT911_REG_CONFIG            0x8047
#define GT911_REG_TOUCH_STATUS      0x814E   /* [7]=buf_rdy [3:0]=count */
#define GT911_REG_TOUCH_DATA        0x814F

/* ========================================================================
 * Color helpers (RGB565)
 * ======================================================================== */

#define rgb565(r, g, b) \
    ((uint16_t)(((uint8_t)(r) & 0xF8) << 8) | \
     ((uint16_t)((uint8_t)(g) & 0xFC) << 3) | \
     ((uint8_t)(b) >> 3))

#define COLOR_BLACK   rgb565(0,   0,   0)
#define COLOR_WHITE   rgb565(255, 255, 255)
#define COLOR_RED     rgb565(255, 0,   0)
#define COLOR_GREEN   rgb565(0,   255, 0)
#define COLOR_BLUE    rgb565(0,   0,   255)
#define COLOR_YELLOW  rgb565(255, 255, 0)
#define COLOR_CYAN    rgb565(0,   255, 255)
#define COLOR_MAGENTA rgb565(255, 0,   255)
#define COLOR_GRAY    rgb565(128, 128, 128)
#define COLOR_DARK_GRAY rgb565(64, 64, 64)
#define COLOR_ORANGE  rgb565(255, 165, 0)

/* Touch point draw colors (cycles through these) */
static const uint16_t TOUCH_COLORS[] = {
    COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_MAGENTA, COLOR_CYAN
};
#define NUM_TOUCH_COLORS (sizeof(TOUCH_COLORS) / sizeof(TOUCH_COLORS[0]))

/* ========================================================================
 * Drawing Primitives (software framebuffer)
 * ======================================================================== */

static void fill_rect(uint16_t *buf, int bx, int by, int bw, int bh, uint16_t color)
{
    if (bx < 0) { bw += bx; bx = 0; }
    if (by < 0) { bh += by; by = 0; }
    if (bx + bw > LCD_W) bw = LCD_W - bx;
    if (by + bh > LCD_H) bh = LCD_H - by;
    if (bw <= 0 || bh <= 0) return;

    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            buf[y * LCD_W + x] = color;
        }
    }
}

static void fill_screen(uint16_t *buf, uint16_t color)
{
    fill_rect(buf, 0, 0, LCD_W, LCD_H, color);
}

static void draw_rect(uint16_t *buf, int bx, int by, int bw, int bh, uint16_t color)
{
    fill_rect(buf, bx, by, bw, 1, color);              /* top */
    fill_rect(buf, bx, by + bh - 1, bw, 1, color);     /* bottom */
    fill_rect(buf, bx, by, 1, bh, color);              /* left */
    fill_rect(buf, bx + bw - 1, by, 1, bh, color);     /* right */
}

static void draw_circle(uint16_t *buf, int cx, int cy, int r, uint16_t color)
{
    /* Integer midpoint circle fill — no floating point */
    int rsq = r * r;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= LCD_H) continue;
        int dy = y - cy;
        int dysq = dy * dy;
        /* Find widest x for this y: x^2 = r^2 - dy^2, scan inward */
        int dx = 0;
        while (dx <= r) {
            if (dx * dx + dysq <= rsq) dx++;
            else break;
        }
        if (dx > 0) dx--;
        int x0 = cx - dx;
        int x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_W) x1 = LCD_W - 1;
        for (int x = x0; x <= x1; x++) {
            buf[y * LCD_W + x] = color;
        }
    }
}

static void draw_crosshair(uint16_t *buf, int cx, int cy, int size, uint16_t color)
{
    int hs = size / 2;
    /* Horizontal */
    fill_rect(buf, cx - hs, cy - 1, size, 3, color);
    /* Vertical */
    fill_rect(buf, cx - 1, cy - hs, 3, size, color);
}

/* ========================================================================
 * 8x16 Bitmap Font (ASCII 32–126)
 * ======================================================================== */

#define FONT_W  8
#define FONT_H  16

/* The font bitmap data is exactly the same as t_display.c.
 * 95 glyphs × 16 bytes = 1520 bytes */
static const uint8_t _font_8x16[][16] = {
    [0]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  32 ' ' */
    [1]  = {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /*  33 '!' */
    [2]  = {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  34 '"' */
    [3]  = {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00}, /*  35 '#' */
    [4]  = {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00}, /*  36 '$' */
    [5]  = {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00}, /*  37 '%' */
    [6]  = {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, /*  38 '&' */
    [7]  = {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  39 ''' */
    [8]  = {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00}, /*  40 '(' */
    [9]  = {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00}, /*  41 ')' */
    [10] = {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, /*  42 '*' */
    [11] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /*  43 '+' */
    [12] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00}, /*  44 ',' */
    [13] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  45 '-' */
    [14] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /*  46 '.' */
    [15] = {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00}, /*  47 '/' */
    [16] = {0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00}, /*  48 '0' */
    [17] = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00}, /*  49 '1' */
    [18] = {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00}, /*  50 '2' */
    [19] = {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  51 '3' */
    [20] = {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00}, /*  52 '4' */
    [21] = {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  53 '5' */
    [22] = {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  54 '6' */
    [23] = {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00}, /*  55 '7' */
    [24] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  56 '8' */
    [25] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00}, /*  57 '9' */
    [26] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /*  58 ':' */
    [27] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00}, /*  59 ';' */
    [28] = {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00}, /*  60 '<' */
    [29] = {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  61 '=' */
    [30] = {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00}, /*  62 '>' */
    [31] = {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /*  63 '?' */
    [32] = {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00}, /*  64 '@' */
    [33] = {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /*  65 'A' */
    [34] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00}, /*  66 'B' */
    [35] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00}, /*  67 'C' */
    [36] = {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00}, /*  68 'D' */
    [37] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00}, /*  69 'E' */
    [38] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /*  70 'F' */
    [39] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00}, /*  71 'G' */
    [40] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /*  72 'H' */
    [41] = {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /*  73 'I' */
    [42] = {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00}, /*  74 'J' */
    [43] = {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, /*  75 'K' */
    [44] = {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0x66,0xFE,0x00,0x00,0x00,0x00}, /*  76 'L' */
    [45] = {0x00,0x00,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00}, /*  77 'M' */
    [46] = {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /*  78 'N' */
    [47] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  79 'O' */
    [48] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /*  80 'P' */
    [49] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00}, /*  81 'Q' */
    [50] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, /*  82 'R' */
    [51] = {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  83 'S' */
    [52] = {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /*  84 'T' */
    [53] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  85 'U' */
    [54] = {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00}, /*  86 'V' */
    [55] = {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0x00,0x00,0x00,0x00}, /*  87 'W' */
    [56] = {0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00,0x00}, /*  88 'X' */
    [57] = {0x00,0x00,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /*  89 'Y' */
    [58] = {0x00,0x00,0xFF,0xC3,0x86,0x0C,0x18,0x30,0x60,0xC1,0xC3,0xFF,0x00,0x00,0x00,0x00}, /*  90 'Z' */
    [59] = {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00}, /*  91 '[' */
    [60] = {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00}, /*  92 '\' */
    [61] = {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00}, /*  93 ']' */
    [62] = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  94 '^' */
    [63] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00}, /*  95 '_' */
    [64] = {0x00,0x60,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*  96 '`' */
    [65] = {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, /*  97 'a' */
    [66] = {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00}, /*  98 'b' */
    [67] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, /*  99 'c' */
    [68] = {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, /* 100 'd' */
    [69] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 101 'e' */
    [70] = {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /* 102 'f' */
    [71] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00}, /* 103 'g' */
    [72] = {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, /* 104 'h' */
    [73] = {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /* 105 'i' */
    [74] = {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00}, /* 106 'j' */
    [75] = {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00}, /* 107 'k' */
    [76] = {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /* 108 'l' */
    [77] = {0x00,0x00,0x00,0x00,0x00,0xE6,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0x00,0x00,0x00,0x00}, /* 109 'm' */
    [78] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00}, /* 110 'n' */
    [79] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 111 'o' */
    [80] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 112 'p' */
    [81] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00}, /* 113 'q' */
    [82] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /* 114 'r' */
    [83] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 115 's' */
    [84] = {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00}, /* 116 't' */
    [85] = {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, /* 117 'u' */
    [86] = {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00}, /* 118 'v' */
    [87] = {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x00,0x00,0x00,0x00}, /* 119 'w' */
    [88] = {0x00,0x00,0x00,0x00,0x00,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00}, /* 120 'x' */
    [89] = {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00}, /* 121 'y' */
    [90] = {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00}, /* 122 'z' */
    [91] = {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00}, /* 123 '{' */
    [92] = {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00}, /* 124 '|' */
    [93] = {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00}, /* 125 '}' */
    [94] = {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 '~' */
};

static const uint8_t *font_get_glyph(char c)
{
    if (c < 32 || c > 126) c = '?';
    return _font_8x16[(int)(c - 32)];
}

static void draw_char_bg(uint16_t *buf, int x, int y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < FONT_H; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col, py = y + row;
            if (px < 0 || px >= LCD_W || py < 0 || py >= LCD_H) continue;
            buf[py * LCD_W + px] = (line & (0x80 >> col)) ? fg : bg;
        }
    }
}

static void draw_string(uint16_t *buf, int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    int cx = x;
    while (*str) {
        if (*str == '\n') { cx = x; y += FONT_H; }
        else { draw_char_bg(buf, cx, y, *str, fg, bg); cx += FONT_W; }
        str++;
    }
}

/* ========================================================================
 * Board Init
 * ======================================================================== */

static void board_power_on(void)
{
    ESP_LOGI(TAG, "--- Board Power Init ---");
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_LCD_POWER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LCD_POWER, 1);
    ESP_LOGI(TAG, "  LCD_POWER (GPIO%d) -> HIGH", PIN_LCD_POWER);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Combined reset: resets both LCD and touch IC.
 * GT911 reset sequence: RST+INT timing controls I2C address selection.
 * After reset, GT911 pulls INT LOW to signal readiness. */
static void combined_hw_reset(void)
{
    ESP_LOGI(TAG, "--- Combined HW Reset (LCD + Touch) ---");

    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_TP_RST) | BIT64(PIN_TP_INT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);

    /* H7 driver reset sequence: 200ms delays are CRITICAL */
    gpio_set_level(PIN_TP_INT, 0);
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(PIN_TP_INT, 1);
    gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(PIN_TP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "  Reset done — INT(GPIO%d)=%d", PIN_TP_INT,
             gpio_get_level(PIN_TP_INT));
}

/* ========================================================================
 * Display (ST7789V via SPI2)
 * ======================================================================== */

static esp_lcd_panel_handle_t display_init(void)
{
    ESP_LOGI(TAG, "--- SPI Bus Init ---");
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_BUF_SIZE + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_bus_cfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "  SPI2: SCK=%d MOSI=%d MISO=%d",
             PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO);

    ESP_LOGI(TAG, "--- Panel IO Init ---");
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                              &io_cfg, &io_handle));

    ESP_LOGI(TAG, "--- ST7789 Panel Init ---");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,   /* We handle reset in combined_hw_reset() —
                                            GPIO8 is shared with GT911, must NOT
                                            be toggled again after touch reset */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));

    /* NOTE: skip esp_lcd_panel_reset() — GPIO8 reset already done in
     * combined_hw_reset() which correctly sequences both LCD and GT911 */
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_LOGI(TAG, "  ST7789V ready — %dx%d RGB565", LCD_W, LCD_H);

    return panel_handle;
}

/* ========================================================================
 * Touch (GT911 via I2C0)
 * ======================================================================== */

/* Write a 16-bit register address then read data. Returns true on success. */
static bool i2c_read_reg16(i2c_master_dev_handle_t dev, uint16_t reg,
                           uint8_t *data, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    esp_err_t err = i2c_master_transmit_receive(dev, reg_buf, 2, data, len,
                                                 pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_read_reg16(0x%04X) failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Write a 16-bit register address with data. */
static bool i2c_write_reg16(i2c_master_dev_handle_t dev, uint16_t reg,
                            const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(2 + len);
    if (!buf) return false;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(buf + 2, data, len);
    esp_err_t err = i2c_master_transmit(dev, buf, 2 + len,
                                         pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_write_reg16(0x%04X) failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ========================================================================
 * GT911 Configuration
 *
 * GT911 requires a valid config table before it starts scanning for touches.
 * Config lives at register 0x8047: version(1) + data(183) + checksum1(1) = 185
 * bytes total. Additional overall checksum at 0x8100.
 *
 * Without valid config, all regs read 0x00 and the status register (0x814E)
 * never reports touch data.
 * ======================================================================== */

/* ========================================================================
 * Uses generous timeouts — GT911 can be slow to wake after reset. */
static bool touch_probe(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *dev_out,
                        uint8_t *addr_out)
{
    /* GT911 possible I2C addresses after reset (depends on INT level during RST↑):
     *   INT=LOW  → 0x5D (or 0xBA)
     *   INT=HIGH → 0x14 (or 0x28)
     * We try both and also a few alternative addresses. */
    const uint8_t addrs[] = { GT911_ADDR_1, GT911_ADDR_2, 0xBA, 0x28 };

    for (int i = 0; i < sizeof(addrs); i++) {
        uint8_t addr = addrs[i];

        /* Use long timeout — GT911 needs time after reset (observed: ~100ms works,
         * 50ms does not on this hardware) */
        esp_err_t probe_err = i2c_master_probe(bus, addr, pdMS_TO_TICKS(120));
        if (probe_err != ESP_OK) continue;

        ESP_LOGI(TAG, "  Device ACK at 0x%02X — probing for GT911...", addr);

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
            ESP_LOGW(TAG, "  Failed to add device at 0x%02X", addr);
            continue;
        }

        /* Read product ID register (0x8140) */
        uint8_t pid[4] = {0};
        if (i2c_read_reg16(dev, GT911_REG_PRODUCT_ID, pid, 4)) {
            ESP_LOGI(TAG, "  Product ID: '%c%c%c%c' (0x%02X %02X %02X %02X)",
                     pid[0], pid[1], pid[2], pid[3],
                     pid[0], pid[1], pid[2], pid[3]);

            if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
                ESP_LOGI(TAG, "  ** GT911 CONFIRMED at 0x%02X **", addr);
                *dev_out = dev;
                *addr_out = addr;
                return true;
            }

            /* Some GT911 variants use different PID strings */
            if (pid[0] >= '0' && pid[0] <= '9' &&
                pid[1] >= '0' && pid[1] <= '9') {
                ESP_LOGI(TAG, "  ** Looks like GT911 variant at 0x%02X **", addr);
                *dev_out = dev;
                *addr_out = addr;
                return true;
            }
        }
        i2c_master_bus_rm_device(dev);
    }

    return false;
}

static bool touch_init(i2c_master_bus_handle_t *bus_out,
                       i2c_master_dev_handle_t *dev_out)
{
    ESP_LOGI(TAG, "--- I2C Bus Init ---");

    /* Quick diagnostic: check INT level. GT911 should pull INT LOW when ready
     * for communication after reset. If HIGH, either GT911 is absent or still
     * initializing (it can take up to 300ms after reset). */
    int int_level = gpio_get_level(PIN_TP_INT);
    ESP_LOGI(TAG, "  INT(GPIO%d) level: %d (%s)",
             PIN_TP_INT, int_level,
             int_level == 0 ? "LOW, GT911 signaling ready" :
             "HIGH (GT911 may still be initializing or absent)");

    /* Init I2C bus with internal pull-ups.
     * Internal ~45kΩ pull-ups work on Leisound V1 with short PCB traces. */
    ESP_LOGI(TAG, "  Initializing I2C0 @ %d kHz...", I2C_MASTER_FREQ_HZ / 1000);
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
    ESP_LOGI(TAG, "  I2C0 ready: SDA=GPIO%d SCL=GPIO%d",
             PIN_I2C_SDA, PIN_I2C_SCL);

    /* Allow bus to settle after init */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Probe GT911 at known addresses */
    i2c_master_dev_handle_t touch_dev = NULL;
    uint8_t touch_addr = 0;

    if (!touch_probe(i2c_bus, &touch_dev, &touch_addr)) {
        /* One retry — sometimes the first probe fails if GT911
         * hasn't finished internal init yet */
        ESP_LOGI(TAG, "  First attempt failed, waiting 200ms then retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));

        if (!touch_probe(i2c_bus, &touch_dev, &touch_addr)) {
            ESP_LOGE(TAG, "!! GT911 not found at 0x%02X, 0x%02X, 0xBA, or 0x28",
                     GT911_ADDR_1, GT911_ADDR_2);
            ESP_LOGE(TAG, "   INT(GPIO%d)=%d  — LOW means GT911 signaled ready but I2C failed",
                     PIN_TP_INT, int_level);
            *bus_out = i2c_bus;
            *dev_out = NULL;
            return false;
        }
    }

    /* Read product ID and config version (H7 init sequence) */
    uint8_t info[4] = {0};
    i2c_read_reg16(touch_dev, GT911_REG_PRODUCT_ID, info, 3);
    vTaskDelay(pdMS_TO_TICKS(200));
    i2c_read_reg16(touch_dev, GT911_REG_CONFIG, &info[3], 1);
    ESP_LOGI(TAG, "  TouchPad_ID: %c,%c,%c  Config: 0x%02X",
             info[0], info[1], info[2], info[3]);

    /* Read firmware version */
    uint8_t fw[2] = {0};
    i2c_read_reg16(touch_dev, GT911_REG_FIRMWARE_VERSION, fw, 2);
    ESP_LOGI(TAG, "  Firmware: 0x%04X", ((uint16_t)fw[1] << 8) + fw[0]);

    /* NOTE: do NOT write config — factory config (ver 0x41) is valid */

    *bus_out = i2c_bus;
    *dev_out = touch_dev;
    return true;
}

/* ========================================================================
 * Touch Visualization — updates framebuffer, does NOT flush to LCD
 * ======================================================================== */

/* Touch data for one point */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t size;
} touch_point_t;

/* Clear touch area (bottom portion of screen) and redraw UI frame.
 * Keeps the top status bar intact. */
static void touch_clear_area(uint16_t *buf)
{
    /* Fill the draw area (below the info bar) with black */
    int info_bar_h = 40;
    fill_rect(buf, 0, info_bar_h, LCD_W, LCD_H - info_bar_h, COLOR_BLACK);

    /* Thin separator line */
    fill_rect(buf, 0, info_bar_h, LCD_W, 2, COLOR_DARK_GRAY);
}

/* Draw the static UI frame (top info bar) */
static void draw_ui_frame(uint16_t *buf)
{
    /* Top bar background */
    fill_rect(buf, 0, 0, LCD_W, 40, COLOR_DARK_GRAY);
    draw_string(buf, 4, 4, "Touch Test", COLOR_WHITE, COLOR_DARK_GRAY);
    draw_string(buf, LCD_W - 130, 4, "GT911 | ST7789", COLOR_GRAY, COLOR_DARK_GRAY);
    draw_string(buf, LCD_W - 100, 20, "Leisound V1", COLOR_GRAY, COLOR_DARK_GRAY);

    /* Separator */
    fill_rect(buf, 0, 38, LCD_W, 2, COLOR_BLUE);

    /* Bottom info area */
    draw_string(buf, 4, LCD_H - 22, "Touch anywhere on screen...", COLOR_DARK_GRAY, COLOR_BLACK);
}

/* Update the touch info text showing current state */
static void draw_touch_info(uint16_t *buf, int num_touches, const touch_point_t *points)
{
    /* Clear info area (below touch area, thin bar at bottom) */
    int info_y = LCD_H - 22;
    fill_rect(buf, 0, info_y, LCD_W, FONT_H + 4, COLOR_BLACK);

    if (num_touches == 0) {
        draw_string(buf, 4, info_y + 2, "No touch  |  Waiting...",
                    COLOR_DARK_GRAY, COLOR_BLACK);
        return;
    }

    /* Show first point details */
    char line[64];
    snprintf(line, sizeof(line), "P%d: %3d,%3d  size=%3d  points=%d",
             0, points[0].x, points[0].y, points[0].size, num_touches);
    draw_string(buf, 4, info_y + 2, line, COLOR_GREEN, COLOR_BLACK);
}

/* Draw touch points as colored circles with crosshairs */
static void draw_touch_points(uint16_t *buf, int num_touches, const touch_point_t *points)
{
    for (int i = 0; i < num_touches; i++) {
        uint16_t color = TOUCH_COLORS[i % NUM_TOUCH_COLORS];
        int cx = points[i].x;
        int cy = points[i].y;
        int r = points[i].size / 2;
        if (r < 8) r = 8;    /* Minimum visible radius */
        if (r > 40) r = 40;  /* Clamp max radius */

        /* Filled circle */
        draw_circle(buf, cx, cy, r, color);

        /* Crosshair */
        draw_crosshair(buf, cx, cy, r * 2 + 4, COLOR_WHITE);

        /* Label: point index */
        draw_char_bg(buf, cx + r + 4, cy - 8, '0' + i, COLOR_WHITE, COLOR_BLACK);

        /* Coordinate label */
        char coord[16];
        snprintf(coord, sizeof(coord), "%d,%d", points[i].x, points[i].y);
        draw_string(buf, cx + r + 4, cy + 2, coord, COLOR_YELLOW, COLOR_BLACK);
    }
}

/* ========================================================================
 * Main touch polling loop with display
 * ======================================================================== */

static void touch_display_loop(esp_lcd_panel_handle_t panel, uint16_t *fb,
                                i2c_master_dev_handle_t touch_dev)
{
    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "  Touch + Display interactive mode");
    ESP_LOGI(TAG, "  Touch screen to draw — watch the UART log too");
    ESP_LOGI(TAG, "======================================================");

    /* Draw the static UI frame once */
    fill_screen(fb, COLOR_BLACK);
    draw_ui_frame(fb);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);

    TickType_t last_flush = xTaskGetTickCount();
    bool needs_flush = false;

    while (1) {
        /* GT911_Scan() — ported from H7 driver */
        uint8_t buf[41] = {0};
        uint8_t Clr = 0;

        /* Read status byte */
        if (!i2c_read_reg16(touch_dev, GT911_REG_TOUCH_STATUS, buf, 1)) {
            i2c_write_reg16(touch_dev, GT911_REG_TOUCH_STATUS, &Clr, 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if ((buf[0] & 0x80) == 0x00) {
            /* No data — always clear status */
            i2c_write_reg16(touch_dev, GT911_REG_TOUCH_STATUS, &Clr, 1);
        } else {
            int count = buf[0] & 0x0F;
            if (count > 5 || count == 0) {
                i2c_write_reg16(touch_dev, GT911_REG_TOUCH_STATUS, &Clr, 1);
            } else {
                /* Read touch points */
                uint8_t raw[40] = {0};
                if (i2c_read_reg16(touch_dev, GT911_REG_TOUCH_DATA, raw, count * 8)) {
                    i2c_write_reg16(touch_dev, GT911_REG_TOUCH_STATUS, &Clr, 1);

                    /* GT911 point format (8 bytes each, starting at 0x814F):
                     *   [0]=track_id [1]=X_lo [2]=X_hi
                     *   [3]=Y_lo [4]=Y_hi [5]=S_lo [6]=S_hi [7]=reserved */
                    touch_point_t points[5];
                    for (int i = 0; i < count; i++) {
                        uint8_t *tp = &raw[i * 8];
                        points[i].x    = ((uint16_t)tp[2] << 8) | tp[1];
                        points[i].y    = ((uint16_t)tp[4] << 8) | tp[3];
                        points[i].size = ((uint16_t)tp[6] << 8) | tp[5];

                        ESP_LOGI(TAG, "TOUCH %d: X=%d Y=%d S=%d",
                                 i, points[i].x, points[i].y, points[i].size);
                    }

                    /* Update display */
                    touch_clear_area(fb);
                    draw_touch_points(fb, count, points);
                    draw_touch_info(fb, count, points);
                    needs_flush = true;
                } else {
                    i2c_write_reg16(touch_dev, GT911_REG_TOUCH_STATUS, &Clr, 1);
                }
            }
        }

        /* Flush framebuffer to LCD at ~30 Hz */
        if (needs_flush) {
            TickType_t now = xTaskGetTickCount();
            if (pdTICKS_TO_MS(now - last_flush) >= 33) {
                esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
                last_flush = now;
                needs_flush = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ========================================================================
 * Initial screen test patterns (brief, then enter touch loop)
 * ======================================================================== */

static void show_splash(esp_lcd_panel_handle_t panel, uint16_t *fb, bool touch_ok)
{
    ESP_LOGI(TAG, "--- Splash Screen ---");

    fill_screen(fb, COLOR_BLACK);

    /* Title */
    draw_rect(fb, 2, 2, LCD_W - 4, LCD_H - 4, COLOR_WHITE);
    draw_rect(fb, 3, 3, LCD_W - 6, LCD_H - 6, COLOR_BLUE);

    fill_rect(fb, 5, 5, LCD_W - 10, 50, COLOR_BLUE);
    draw_string(fb, LCD_W / 2 - 64, 15, "Leisound V1", COLOR_WHITE, COLOR_BLUE);
    draw_string(fb, LCD_W / 2 - 80, 33, "Display + Touch Test", COLOR_YELLOW, COLOR_BLUE);

    /* Hardware info */
    int y = 65;
    draw_string(fb, 15, y, "Display: ST7789V @ SPI2", COLOR_GREEN, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, "  240x320  RGB565  20MHz", COLOR_WHITE, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, "", COLOR_WHITE, COLOR_BLACK); y += 10;

    draw_string(fb, 15, y, "Touch: GT911 @ I2C0", COLOR_GREEN, COLOR_BLACK); y += 20;
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "  Addr: 0x%02X / 0x%02X  100kHz",
                 GT911_ADDR_1, GT911_ADDR_2);
        draw_string(fb, 15, y, buf, COLOR_WHITE, COLOR_BLACK); y += 20;
    }

    /* Pin map */
    y += 10;
    fill_rect(fb, 10, y, LCD_W - 20, 2, COLOR_WHITE); y += 12;
    draw_string(fb, 15, y, "Pin Map:", COLOR_YELLOW, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, "  SPI:  SCK=12 MOSI=11 CS=10 DC=45 RST=8",
                COLOR_WHITE, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, "  I2C:  SDA=47  SCL=48  INT=18",
                COLOR_WHITE, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, "  PWR:  EN=46", COLOR_WHITE, COLOR_BLACK); y += 20;

    /* Status */
    y += 10;
    fill_rect(fb, 10, y, LCD_W - 20, 2, COLOR_WHITE); y += 12;
    draw_string(fb, 15, y, "Status:", COLOR_YELLOW, COLOR_BLACK); y += 22;

    draw_string(fb, 15, y, "  Display ................... OK", COLOR_GREEN, COLOR_BLACK); y += 20;
    draw_string(fb, 15, y, touch_ok ? "  Touch ..................... OK" : "  Touch ..................... FAIL",
                touch_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK); y += 20;

    /* Footer */
    y += 10;
    fill_rect(fb, 5, LCD_H - 35, LCD_W - 10, 30, COLOR_GRAY);
    if (touch_ok) {
        draw_string(fb, LCD_W / 2 - 95, LCD_H - 28, "STARTING TOUCH TEST...",
                    COLOR_BLACK, COLOR_GRAY);
    } else {
        draw_string(fb, LCD_W / 2 - 75, LCD_H - 28, "TOUCH NOT FOUND",
                    COLOR_RED, COLOR_GRAY);
    }

    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* ========================================================================
 * Main Entry
 * ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "  Leisound V1 — Display + Touch Combined Test");
    ESP_LOGI(TAG, "  ST7789V (SPI2) + GT911 (I2C0)");
    ESP_LOGI(TAG, "  Pin defs ref: boards/espressif/leisound_v1 GT911");
    ESP_LOGI(TAG, "======================================================");

    /* --- 1. Board power on --- */
    board_power_on();

    /* --- 2. Combined hardware reset (LCD + Touch share GPIO8 RST) --- */
    combined_hw_reset();

    /* --- 3. Initialize display --- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===== DISPLAY INIT =====");
    esp_lcd_panel_handle_t panel = display_init();

    /* --- 4. Allocate frame buffer (PSRAM preferred) --- */
    uint16_t *fb = heap_caps_malloc(LCD_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        fb = heap_caps_malloc(LCD_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!fb) {
        ESP_LOGE(TAG, "FATAL: Cannot allocate frame buffer (%d bytes)!", LCD_BUF_SIZE);
        goto fail;
    }
    ESP_LOGI(TAG, "  Frame buffer: %d bytes @ %p", LCD_BUF_SIZE, fb);

    /* --- 5. Initialize touch --- */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===== TOUCH INIT =====");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_dev_handle_t touch_dev = NULL;
    bool touch_ok = touch_init(&i2c_bus, &touch_dev);

    /* --- 6. Show splash screen --- */
    ESP_LOGI(TAG, "");
    show_splash(panel, fb, touch_ok);

    /* --- 7. Enter touch loop (or display-only standby) --- */
    if (touch_ok) {
        touch_display_loop(panel, fb, touch_dev);
    } else {
        ESP_LOGW(TAG, "Touch not detected — entering display-only mode");
        ESP_LOGW(TAG, "Check:");
        ESP_LOGW(TAG, "  1. Is GT911 properly soldered?");
        ESP_LOGW(TAG, "  2. Are I2C lines connected? (GPIO47=SDA, GPIO48=SCL)");
        ESP_LOGW(TAG, "  3. Is the shared RST (GPIO8) working for touch?");

        /* Display a message on screen */
        fill_screen(fb, COLOR_BLACK);
        draw_ui_frame(fb);
        int my = LCD_H / 2 - 40;
        draw_string(fb, 20, my, "TOUCH NOT DETECTED", COLOR_RED, COLOR_BLACK); my += 24;
        draw_string(fb, 20, my, "Check I2C wiring:", COLOR_YELLOW, COLOR_BLACK); my += 20;
        draw_string(fb, 20, my, "  SDA=GPIO47  SCL=GPIO48", COLOR_WHITE, COLOR_BLACK); my += 20;
        draw_string(fb, 20, my, "  INT=GPIO18  RST=GPIO8", COLOR_WHITE, COLOR_BLACK); my += 28;
        draw_string(fb, 20, my, "Display OK — waiting...", COLOR_GREEN, COLOR_BLACK);
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "STATUS: Display OK, Touch NOT FOUND");
        }
    }

    return;

fail:
    ESP_LOGE(TAG, "DISPLAY+TOUCH TEST FAILED");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
