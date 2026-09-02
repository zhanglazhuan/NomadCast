/*
 * t_ui — Proven display init + LVGL (system malloc, no ADF, no custom pools)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "t_ui";

#define PIN_EN_POWER    GPIO_NUM_46
#define PIN_SPI_SCK     GPIO_NUM_12
#define PIN_SPI_MOSI    GPIO_NUM_11
#define PIN_SPI_MISO    GPIO_NUM_13
#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_45
#define PIN_LCD_RST     GPIO_NUM_8
#define PIN_TP_INT      GPIO_NUM_18
#define PIN_TP_RST      GPIO_NUM_8
#define PIN_I2C_SDA     GPIO_NUM_47
#define PIN_I2C_SCL     GPIO_NUM_48
#define LCD_W           240
#define LCD_H           320
#define LCD_BUF_SIZE    (LCD_W * LCD_H * sizeof(uint16_t))
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (20 * 1000 * 1000)

static esp_lcd_panel_handle_t display_init(void)
{
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_BUF_SIZE + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = SPI_FREQ_HZ, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    return panel;
}

static void combined_hw_reset(void)
{
    gpio_config_t cfg = { .pin_bit_mask = BIT64(PIN_TP_RST) | BIT64(PIN_TP_INT), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(PIN_TP_INT, 0); gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_INT, 1);
    gpio_config_t int_cfg = { .pin_bit_mask = BIT64(PIN_TP_INT), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    /* RGB565 byte swap — LVGL big-endian → ST7789 SPI little-endian */
    lv_draw_sw_rgb565_swap(px_map, w * h);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x1 + w, area->y1 + h, px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(1); }

/* ========================================================================
 * Touch (GT911 via I2C0) — from t_display_touch
 * ======================================================================== */
#define I2C_FREQ_HZ   (100 * 1000)
#define GT911_ADDR    0x5D
#define GT911_REG_STATUS  0x814E
#define GT911_REG_DATA    0x814F

static i2c_master_dev_handle_t touch_dev;

static bool i2c_read_reg16(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t rb[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(dev, rb, 2, data, len, pdMS_TO_TICKS(100)) == ESP_OK;
}

static bool i2c_write_reg16(i2c_master_dev_handle_t dev, uint16_t reg, const uint8_t *d, size_t len)
{
    uint8_t *buf = malloc(2 + len);
    if (!buf) return false;
    buf[0] = reg >> 8; buf[1] = reg & 0xFF;
    memcpy(buf + 2, d, len);
    esp_err_t r = i2c_master_transmit(dev, buf, 2 + len, pdMS_TO_TICKS(100));
    free(buf);
    return r == ESP_OK;
}

static bool touch_probe(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *dev_out)
{
    if (i2c_master_probe(bus, GT911_ADDR, pdMS_TO_TICKS(200)) != ESP_OK) return false;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GT911_ADDR, .scl_speed_hz = I2C_FREQ_HZ};
    if (i2c_master_bus_add_device(bus, &dc, dev_out) != ESP_OK) return false;
    uint8_t pid[4] = {0};
    i2c_read_reg16(*dev_out, 0x8140, pid, 4);
    if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') return true;
    i2c_master_bus_rm_device(*dev_out);
    return false;
}

static esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "Touch init (GT911, I2C0)...");
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "I2C bus");
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!touch_probe(bus, &touch_dev)) {
        vTaskDelay(pdMS_TO_TICKS(200)); /* retry once */
        if (!touch_probe(bus, &touch_dev)) { ESP_LOGE(TAG, "GT911 not found"); return ESP_ERR_NOT_FOUND; }
    }
    ESP_LOGI(TAG, "GT911 ready");
    return ESP_OK;
}

/* ---- Swipe detection state ---- */
static int32_t  swipe_start_x, swipe_last_x;
static bool     swipe_tracking;

/* LVGL touch indev callback — also detects screen-level swipes */
static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t buf[41] = {0}, clear = 0;
    if (!i2c_read_reg16(touch_dev, GT911_REG_STATUS, buf, 1)) {
        goto released;
    }
    if ((buf[0] & 0x80) == 0) {
        i2c_write_reg16(touch_dev, GT911_REG_STATUS, &clear, 1);
        goto released;
    }
    int n = buf[0] & 0x0F;
    if (n == 0 || n > 5) {
        i2c_write_reg16(touch_dev, GT911_REG_STATUS, &clear, 1);
        goto released;
    }
    uint8_t raw[40];
    if (!i2c_read_reg16(touch_dev, GT911_REG_DATA, raw, n * 8)) {
        goto released;
    }
    i2c_write_reg16(touch_dev, GT911_REG_STATUS, &clear, 1);

    uint16_t x = ((uint16_t)raw[2] << 8) | raw[1];
    uint16_t y = ((uint16_t)raw[4] << 8) | raw[3];

    /* Track swipe start + last position */
    if (!swipe_tracking) {
        swipe_start_x = x;
        swipe_tracking = true;
    }
    swipe_last_x = x;

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
    return;

released:
    if (swipe_tracking) {
        int32_t dx = (int32_t)swipe_last_x - swipe_start_x;
        if (dx < -50)  ESP_LOGI(TAG, "Left swipe detected");
        if (dx > 50)   ESP_LOGI(TAG, "Right swipe detected");
        swipe_tracking = false;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_ui: LVGL on proven base ===");

    /* 1. Power */
    gpio_config_t pwr = { .pin_bit_mask = BIT64(PIN_EN_POWER), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pwr); gpio_set_level(PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 2. HW reset */
    combined_hw_reset();

    /* 3. Display init */
    esp_lcd_panel_handle_t panel = display_init();

    /* 4. LVGL */
    ESP_LOGI(TAG, "LVGL init...");
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_user_data(disp, panel);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    size_t buf_sz = LCD_W * 20 * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb, .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);  /* 1000 us = 1 ms */

    /* 5. Touch */
    if (touch_init() == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, lvgl_touch_cb);
        ESP_LOGI(TAG, "Touch indev registered");
    }

    /* 6. UI */
    LV_FONT_DECLARE(lv_font_montserrat_14);
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                          LV_THEME_DEFAULT_DARK, &lv_font_montserrat_14);
    ui_init();

    ESP_LOGI(TAG, "Loop...");
    while (1) {
        uint32_t delay = lv_timer_handler();
        if (delay < 5) delay = 5;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
