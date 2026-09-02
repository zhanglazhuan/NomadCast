/*
 * Leisound V1 — Display + Touch Board HAL Implementation
 *
 * ST7789V (240x320 RGB565) via SPI2 @ 20MHz
 * GT911 via I2C0 @ 100kHz (manual driver, matching t_display_touch exactly)
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_ui.h"

static const char *TAG = "board_ui";

#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (20 * 1000 * 1000)
#define I2C_FREQ_HZ     (100 * 1000)
#define I2C_TIMEOUT_MS  100

/* ---- GT911 register addresses ---- */
#define GT911_REG_PRODUCT_ID    0x8140
#define GT911_REG_CONFIG        0x8047
#define GT911_REG_FW_VERSION    0x8144
#define GT911_REG_TOUCH_STATUS  0x814E
#define GT911_REG_TOUCH_DATA    0x814F

/* ---- Touch device handle (opaque) ---- */
struct board_ui_touch_dev_t {
    i2c_master_bus_handle_t  i2c_bus;
    i2c_master_dev_handle_t  i2c_dev;
};

/* ---- I2C helpers (16-bit register address) ---- */

static bool i2c_read_reg16(i2c_master_dev_handle_t dev, uint16_t reg,
                           uint8_t *data, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    esp_err_t err = i2c_master_transmit_receive(dev, reg_buf, 2, data, len,
                                                 pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    return (err == ESP_OK);
}

static bool i2c_write_reg16(i2c_master_dev_handle_t dev, uint16_t reg,
                            const uint8_t *data, size_t len)
{
    uint8_t *buf = malloc(2 + len);
    if (!buf) return false;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(buf + 2, data, len);
    esp_err_t err = i2c_master_transmit(dev, buf, 2 + len,
                                         pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    free(buf);
    return (err == ESP_OK);
}

/* ========================================================================
 * Power
 * ======================================================================== */

esp_err_t board_ui_power_on(void)
{
    ESP_LOGI(TAG, "Power ON: EN_POWER(GPIO%d)=1", BOARD_UI_PIN_EN_POWER);
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BOARD_UI_PIN_EN_POWER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_UI_PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/* ========================================================================
 * Combined Hardware Reset — matches t_display_touch combined_hw_reset()
 *
 * Shared GPIO8: LCD RST + TP RST
 * GT911 I2C address selection depends on INT level during RST rising edge:
 *   INT=LOW  → addr 0x5D/0xBA
 *   INT=HIGH → addr 0x14/0x28
 * This sequence selects 0x5D.
 * ======================================================================== */

esp_err_t board_ui_hw_reset(void)
{
    ESP_LOGI(TAG, "--- Combined HW Reset (LCD + Touch, GPIO8) ---");

    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(BOARD_UI_PIN_TP_RST) | BIT64(BOARD_UI_PIN_TP_INT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    gpio_set_level(BOARD_UI_PIN_TP_INT, 0);
    gpio_set_level(BOARD_UI_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(BOARD_UI_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(BOARD_UI_PIN_TP_INT, 1);
    gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(BOARD_UI_PIN_TP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Reset done — INT(GPIO%d)=%d", BOARD_UI_PIN_TP_INT,
             gpio_get_level(BOARD_UI_PIN_TP_INT));
    return ESP_OK;
}

/* ========================================================================
 * Display — ST7789V via SPI2 (esp_lcd framework)
 *
 * reset_gpio_num = NC: reset already done by board_ui_hw_reset().
 * Matches t_display_touch display_init() exactly.
 * ======================================================================== */

esp_err_t board_ui_display_init(esp_lcd_panel_handle_t *panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    *panel = NULL;

    ESP_LOGI(TAG, "--- Display Init (ST7789V, %dx%d, SPI2 @ %d MHz) ---",
             BOARD_UI_LCD_W, BOARD_UI_LCD_H, SPI_FREQ_HZ / 1000000);

    /* SPI2 bus */
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = BOARD_UI_PIN_SPI_MOSI,
        .miso_io_num = BOARD_UI_PIN_SPI_MISO,
        .sclk_io_num = BOARD_UI_PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_UI_LCD_W * BOARD_UI_LCD_H * sizeof(uint16_t) + 16,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI_HOST_ID, &spi_bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "SPI bus init failed");

    /* Panel IO (SPI 4-wire) */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_UI_PIN_LCD_CS,
        .dc_gpio_num = BOARD_UI_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                  &io_cfg, &io_handle),
        TAG, "Panel IO init failed");

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(io_handle, &panel_cfg, panel),
        TAG, "ST7789 panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel, true), TAG, "Invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "Display on failed");

    ESP_LOGI(TAG, "ST7789V ready");
    return ESP_OK;
}

/* ========================================================================
 * Touch — GT911 via I2C0 (manual driver, matching t_display_touch)
 *
 * Does NOT perform hardware reset — assumes board_ui_hw_reset() was called.
 * ======================================================================== */

esp_err_t board_ui_touch_init(board_ui_touch_dev_t **dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "dev is NULL");
    *dev = NULL;

    ESP_LOGI(TAG, "--- Touch Init (GT911, I2C0 @ %d kHz) ---", I2C_FREQ_HZ / 1000);

    /* Diagnostic: check INT level */
    int int_level = gpio_get_level(BOARD_UI_PIN_TP_INT);
    ESP_LOGI(TAG, "INT(GPIO%d) level: %d (%s)", BOARD_UI_PIN_TP_INT, int_level,
             int_level == 0 ? "LOW, GT911 ready" : "HIGH, may be initializing");

    /* Allocate device handle */
    board_ui_touch_dev_t *d = calloc(1, sizeof(board_ui_touch_dev_t));
    if (!d) return ESP_ERR_NO_MEM;

    /* I2C bus init */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_UI_PIN_I2C_SDA,
        .scl_io_num = BOARD_UI_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &d->i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        free(d);
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Probe GT911 at known addresses */
    const uint8_t addrs[] = { 0x5D, 0x14 };
    bool found = false;

    for (int i = 0; i < 2; i++) {
        esp_err_t probe = i2c_master_probe(d->i2c_bus, addrs[i],
                                            pdMS_TO_TICKS(200));
        if (probe != ESP_OK) continue;

        ESP_LOGI(TAG, "Device ACK at 0x%02X — probing for GT911...", addrs[i]);

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = I2C_FREQ_HZ,
        };
        if (i2c_master_bus_add_device(d->i2c_bus, &dev_cfg, &d->i2c_dev) != ESP_OK) {
            continue;
        }

        /* Read product ID */
        uint8_t pid[4] = {0};
        if (i2c_read_reg16(d->i2c_dev, GT911_REG_PRODUCT_ID, pid, 4)) {
            ESP_LOGI(TAG, "Product ID: '%c%c%c%c'", pid[0], pid[1], pid[2], pid[3]);
            if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
                ESP_LOGI(TAG, "** GT911 CONFIRMED at 0x%02X **", addrs[i]);
                found = true;
                break;
            }
        }
        i2c_master_bus_rm_device(d->i2c_dev);
        d->i2c_dev = NULL;
    }

    if (!found) {
        /* Retry once after delay */
        ESP_LOGI(TAG, "First attempt failed, waiting 200ms then retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));

        for (int i = 0; i < 2; i++) {
            if (i2c_master_probe(d->i2c_bus, addrs[i], pdMS_TO_TICKS(200)) != ESP_OK)
                continue;

            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addrs[i],
                .scl_speed_hz = I2C_FREQ_HZ,
            };
            if (i2c_master_bus_add_device(d->i2c_bus, &dev_cfg, &d->i2c_dev) != ESP_OK)
                continue;

            uint8_t pid[4] = {0};
            if (i2c_read_reg16(d->i2c_dev, GT911_REG_PRODUCT_ID, pid, 4)) {
                if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
                    ESP_LOGI(TAG, "** GT911 CONFIRMED at 0x%02X (retry) **", addrs[i]);
                    found = true;
                    break;
                }
            }
            i2c_master_bus_rm_device(d->i2c_dev);
            d->i2c_dev = NULL;
        }
    }

    if (!found) {
        ESP_LOGE(TAG, "GT911 NOT FOUND on I2C bus");
        i2c_del_master_bus(d->i2c_bus);
        free(d);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read config version */
    vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t cfg_ver = 0;
    i2c_read_reg16(d->i2c_dev, GT911_REG_CONFIG, &cfg_ver, 1);
    ESP_LOGI(TAG, "Config version: 0x%02X", cfg_ver);

    /* Read firmware version */
    uint8_t fw[2] = {0};
    i2c_read_reg16(d->i2c_dev, GT911_REG_FW_VERSION, fw, 2);
    ESP_LOGI(TAG, "Firmware: 0x%04X", ((uint16_t)fw[1] << 8) + fw[0]);

    *dev = d;
    ESP_LOGI(TAG, "GT911 ready");
    return ESP_OK;
}

/* ---- Touch scan (matching t_display_touch scan logic) ---- */

esp_err_t board_ui_touch_scan(board_ui_touch_dev_t *dev,
                              board_ui_touch_data_t *data)
{
    if (!dev || !data) return ESP_ERR_INVALID_ARG;

    uint8_t buf[41] = {0};
    uint8_t clear = 0;

    /* Read status register */
    if (!i2c_read_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, buf, 1)) {
        i2c_write_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, &clear, 1);
        data->count = 0;
        return ESP_OK;
    }

    if ((buf[0] & 0x80) == 0x00) {
        i2c_write_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, &clear, 1);
        data->count = 0;
        return ESP_OK;
    }

    int count = buf[0] & 0x0F;
    if (count > 5 || count == 0) {
        i2c_write_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, &clear, 1);
        data->count = 0;
        return ESP_OK;
    }

    /* Read touch point data (8 bytes per point) */
    uint8_t raw[40] = {0};
    if (!i2c_read_reg16(dev->i2c_dev, GT911_REG_TOUCH_DATA, raw, count * 8)) {
        i2c_write_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, &clear, 1);
        data->count = 0;
        return ESP_OK;
    }

    i2c_write_reg16(dev->i2c_dev, GT911_REG_TOUCH_STATUS, &clear, 1);

    data->count = count;
    for (int i = 0; i < count; i++) {
        uint8_t *tp = &raw[i * 8];
        data->points[i].track_id = tp[0];
        data->points[i].x        = ((uint16_t)tp[2] << 8) | tp[1];
        data->points[i].y        = ((uint16_t)tp[4] << 8) | tp[3];
        data->points[i].size     = ((uint16_t)tp[6] << 8) | tp[5];
    }

    return ESP_OK;
}
