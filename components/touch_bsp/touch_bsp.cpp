/*
 * Touch Panel BSP — GT911 Driver Implementation
 *
 * Ported from the working debug/t_display_touch/ GT911 code.
 * The GT911 uses 16-bit register addresses (unlike the CSTx chip
 * in the reference AMOLED board, which uses 8-bit addresses).
 *
 * GT911 Registers:
 *   0x8140 — Product ID (4 bytes, "911" ASCII)
 *   0x814E — Touch Status: [7]=buf_rdy, [3:0]=point_count
 *   0x814F — Touch Data: N × 8 bytes (track_id, X_lo, X_hi, Y_lo, Y_hi, S_lo, S_hi, reserved)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "touch_bsp.h"
#include "esp_log.h"

static const char *TAG = "Touch";

/* GT911 Register addresses (16-bit big-endian) */
#define GT911_REG_PRODUCT_ID    0x8140
#define GT911_REG_TOUCH_STATUS  0x814E
#define GT911_REG_TOUCH_DATA    0x814F

/* ========================================================================
 * I2C Helpers (16-bit register address)
 * ======================================================================== */

bool LcdTouchPanel::i2c_read_reg16(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    int ret = i2cbus_.i2c_master_write_read_dev(touch_dev_handle_,
                                                  reg_buf, 2, data, len);
    return (ret == ESP_OK);
}

bool LcdTouchPanel::i2c_write_reg16(uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t *buf = (uint8_t *)malloc(2 + len);
    if (!buf) return false;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(buf + 2, data, len);
    int ret = i2cbus_.i2c_write_buff(touch_dev_handle_, -1, buf, 2 + len);
    free(buf);
    return (ret == ESP_OK);
}

/* ========================================================================
 * GT911 Probe
 * ======================================================================== */

bool LcdTouchPanel::probe_gt911(void)
{
    const uint8_t addrs[] = { 0x5D, 0x14, 0xBA, 0x28 };
    i2c_master_bus_handle_t bus = i2cbus_.Get_I2cBusHandle();

    for (int i = 0; i < sizeof(addrs); i++) {
        uint8_t addr = addrs[i];

        esp_err_t probe_err = i2c_master_probe(bus, addr, pdMS_TO_TICKS(120));
        if (probe_err != ESP_OK) continue;

        ESP_LOGI(TAG, "Device ACK at 0x%02X — probing for GT911...", addr);

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addr,
            .scl_speed_hz    = 100000,   /* 100 kHz */
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
            continue;
        }

        /* Temporarily use this device handle for the probe read */
        i2c_master_dev_handle_t saved = touch_dev_handle_;
        touch_dev_handle_ = dev;

        uint8_t pid[4] = {0};
        if (i2c_read_reg16(GT911_REG_PRODUCT_ID, pid, 4)) {
            ESP_LOGI(TAG, "Product ID: '%c%c%c%c'", pid[0], pid[1], pid[2], pid[3]);

            if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
                ESP_LOGI(TAG, "** GT911 CONFIRMED at 0x%02X **", addr);
                dev_addr_ = addr;
                return true;
            }

            /* Accept any numeric PID as GT911 variant */
            if (pid[0] >= '0' && pid[0] <= '9' &&
                pid[1] >= '0' && pid[1] <= '9') {
                ESP_LOGI(TAG, "** GT911 variant at 0x%02X **", addr);
                dev_addr_ = addr;
                return true;
            }
        }

        /* Not GT911 — remove device and restore handle */
        i2c_master_bus_rm_device(dev);
        touch_dev_handle_ = saved;
    }

    return false;
}

/* ========================================================================
 * Constructor / Destructor
 * ======================================================================== */

LcdTouchPanel::LcdTouchPanel(I2cMasterBus& i2cbus, int dev_addr,
                               int touch_rst_pin, int touch_int_pin)
    : i2cbus_(i2cbus),
      touch_dev_handle_(NULL),
      touch_rst_pin_(touch_rst_pin),
      touch_int_pin_(touch_int_pin),
      dev_addr_(dev_addr)
{
    /* Configure RST pin as output with pull-up */
    if (touch_rst_pin_ != GPIO_NUM_NC) {
        gpio_config_t gpio_conf = {};
        gpio_conf.intr_type     = GPIO_INTR_DISABLE;
        gpio_conf.mode          = GPIO_MODE_OUTPUT;
        gpio_conf.pin_bit_mask  = BIT64(touch_rst_pin_);
        gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    }

    /* Configure INT pin (initially output for reset sequence, then input) */
    if (touch_int_pin_ != GPIO_NUM_NC) {
        gpio_config_t gpio_conf = {};
        gpio_conf.intr_type     = GPIO_INTR_DISABLE;
        gpio_conf.mode          = GPIO_MODE_OUTPUT;
        gpio_conf.pin_bit_mask  = BIT64(touch_int_pin_);
        gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    }
}

LcdTouchPanel::~LcdTouchPanel()
{
}

/* ========================================================================
 * GT911 Reset Sequence
 *
 * Critical timing (from H7 GT911 driver):
 *   1. INT=LOW, RST=LOW, wait 200ms
 *   2. RST=HIGH, wait 200ms
 *   3. INT=HIGH, switch INT to input, wait 200ms
 *
 * After reset:
 *   - GT911 pulls INT LOW to signal readiness
 *   - I2C address depends on INT level during RST rising edge:
 *     INT=LOW  → 0x5D (or 0xBA)
 *     INT=HIGH → 0x14 (or 0x28)
 * ======================================================================== */

void LcdTouchPanel::ResetTouch(void)
{
    if (touch_rst_pin_ == GPIO_NUM_NC || touch_int_pin_ == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Reset pins not configured — skipping HW reset");
        return;
    }

    ESP_LOGI(TAG, "GT911 hardware reset...");

    /* Step 1: Drive both pins LOW */
    gpio_set_level((gpio_num_t)touch_int_pin_, 0);
    gpio_set_level((gpio_num_t)touch_rst_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 2: Release RST (INT still LOW → selects 0x5D address) */
    gpio_set_level((gpio_num_t)touch_rst_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 3: Release INT, switch to input with pull-up */
    gpio_set_level((gpio_num_t)touch_int_pin_, 1);

    gpio_config_t int_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT64(touch_int_pin_),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));

    int int_level = gpio_get_level((gpio_num_t)touch_int_pin_);
    ESP_LOGI(TAG, "Reset done — INT(GPIO%d)=%d (%s)",
             touch_int_pin_, int_level,
             int_level == 0 ? "LOW, GT911 ready" : "HIGH");

    /* Probe and register the GT911 I2C device */
    vTaskDelay(pdMS_TO_TICKS(10));   /* Allow I2C bus to settle */

    if (!probe_gt911()) {
        /* One retry after additional delay */
        ESP_LOGI(TAG, "First probe failed, waiting 200ms then retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!probe_gt911()) {
            ESP_LOGE(TAG, "GT911 not found at any known address");
            return;
        }
    }

    ESP_LOGI(TAG, "GT911 initialized at 0x%02X", dev_addr_);
}

/* ========================================================================
 * Touch Coordinate Reading
 *
 * GT911 status register (0x814E):
 *   Bit 7: Buffer ready (1 = data available)
 *   Bits 3-0: Number of touch points (1-5)
 *
 * GT911 point format (8 bytes each, starting at 0x814F):
 *   [0]=track_id  [1]=X_lo  [2]=X_hi
 *   [3]=Y_lo      [4]=Y_hi  [5]=S_lo
 *   [6]=S_hi      [7]=reserved
 *
 * Returns: 1 if touch data valid, 0 otherwise.
 * On success, writes the first touch point's coordinates to *x and *y.
 * ======================================================================== */

uint8_t LcdTouchPanel::GetCoords(uint16_t *x, uint16_t *y)
{
    if (touch_dev_handle_ == NULL) {
        return 0;
    }

    uint8_t status = 0;
    uint8_t clr    = 0;

    if (!i2c_read_reg16(GT911_REG_TOUCH_STATUS, &status, 1)) {
        i2c_write_reg16(GT911_REG_TOUCH_STATUS, &clr, 1);
        return 0;
    }

    if ((status & 0x80) == 0x00) {
        /* No data ready */
        i2c_write_reg16(GT911_REG_TOUCH_STATUS, &clr, 1);
        return 0;
    }

    int count = status & 0x0F;
    if (count == 0 || count > 5) {
        i2c_write_reg16(GT911_REG_TOUCH_STATUS, &clr, 1);
        return 0;
    }

    /* Read touch points */
    uint8_t raw[40] = {0};
    if (!i2c_read_reg16(GT911_REG_TOUCH_DATA, raw, count * 8)) {
        i2c_write_reg16(GT911_REG_TOUCH_STATUS, &clr, 1);
        return 0;
    }

    /* Clear status after reading */
    i2c_write_reg16(GT911_REG_TOUCH_STATUS, &clr, 1);

    /* Extract first touch point */
    uint8_t *tp = &raw[0];
    *x = ((uint16_t)tp[2] << 8) | tp[1];
    *y = ((uint16_t)tp[4] << 8) | tp[3];
    /* size = ((uint16_t)tp[6] << 8) | tp[5]; — available if needed */

    return 1;
}
