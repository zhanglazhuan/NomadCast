/*
 * GT911 Capacitive Touch Controller Driver — Software I2C (bit-banging) variant
 *
 * Replaces the ESP-IDF hardware I2C master driver with pure GPIO bit-banging.
 * Motivation: the V1.1 board routes SCL to GPIO45 (a VDD_SPI strapping pin)
 * whose default pull-down fights the hardware I2C driver's internal pull-up,
 * producing "probe device timeout" / no-ACK. Bit-banging instead drives SCL as
 * a push-pull output (hard 3.3V) and SDA as open-drain with an explicit weak
 * pull-up, overriding the strap.
 *
 * Ported from H7_GT911_I2C_Touch-master (STM32H7) to ESP-IDF.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "gt911.h"

static const char *TAG = "gt911";

/* ---- Device handle (opaque) ---- */

struct gt911_dev_t {
    uint8_t                   addr;        /* 7-bit I2C address (0x5D or 0x14) */
    gpio_num_t                rst_pin;
    gpio_num_t                int_pin;
    uint16_t                  max_width;
    uint16_t                  max_height;

    /* Interrupt mode */
    bool                      use_interrupt;
    SemaphoreHandle_t         irq_sem;
    TaskHandle_t              irq_task;
    gt911_touch_cb_t          touch_cb;
    void                     *cb_user_data;
    volatile bool             suspended;
};

/* ========================================================================
 * Software I2C bit-banging
 * ======================================================================== */

static gpio_num_t s_sw_scl = GPIO_NUM_NC;
static gpio_num_t s_sw_sda = GPIO_NUM_NC;

/* ~100 kHz half-bit delay */
static inline void sw_i2c_delay(void)
{
    esp_rom_delay_us(5);
}

/* Init software I2C pins:
 *   SCL = push-pull output (hard 3.3V drive, ignores strap pull-down)
 *   SDA = open-drain I/O with weak internal pull-up */
static void sw_i2c_init(gpio_num_t scl_pin, gpio_num_t sda_pin)
{
    s_sw_scl = scl_pin;
    s_sw_sda = sda_pin;

    gpio_config_t scl_conf = {
        .pin_bit_mask = BIT64(s_sw_scl),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&scl_conf);

    gpio_config_t sda_conf = {
        .pin_bit_mask = BIT64(s_sw_sda),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sda_conf);

    gpio_set_level(s_sw_scl, 1);
    gpio_set_level(s_sw_sda, 1);
    sw_i2c_delay();
}

static void sw_i2c_start(void)
{
    gpio_set_level(s_sw_sda, 1);
    gpio_set_level(s_sw_scl, 1);
    sw_i2c_delay();
    gpio_set_level(s_sw_sda, 0);
    sw_i2c_delay();
    gpio_set_level(s_sw_scl, 0);
    sw_i2c_delay();
}

static void sw_i2c_stop(void)
{
    gpio_set_level(s_sw_sda, 0);
    gpio_set_level(s_sw_scl, 1);
    sw_i2c_delay();
    gpio_set_level(s_sw_sda, 1);
    sw_i2c_delay();
}

/* Write one byte MSB-first; returns true on ACK. */
static bool sw_i2c_write_byte(uint8_t data)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_level(s_sw_sda, (data & 0x80) ? 1 : 0);
        data <<= 1;
        sw_i2c_delay();
        gpio_set_level(s_sw_scl, 1);
        sw_i2c_delay();
        gpio_set_level(s_sw_scl, 0);
    }
    /* Read ACK */
    gpio_set_level(s_sw_sda, 1);  /* release SDA */
    sw_i2c_delay();
    gpio_set_level(s_sw_scl, 1);
    sw_i2c_delay();
    bool ack = (gpio_get_level(s_sw_sda) == 0);
    gpio_set_level(s_sw_scl, 0);
    sw_i2c_delay();
    return ack;
}

/* Read one byte MSB-first; ack=true ACKs the byte, false NACKs. */
static uint8_t sw_i2c_read_byte(bool ack)
{
    uint8_t data = 0;
    gpio_set_level(s_sw_sda, 1);  /* release SDA for slave output */
    for (int i = 0; i < 8; i++) {
        data <<= 1;
        sw_i2c_delay();
        gpio_set_level(s_sw_scl, 1);
        sw_i2c_delay();
        if (gpio_get_level(s_sw_sda)) {
            data |= 0x01;
        }
        gpio_set_level(s_sw_scl, 0);
    }
    /* Send ACK/NACK */
    gpio_set_level(s_sw_sda, ack ? 0 : 1);
    sw_i2c_delay();
    gpio_set_level(s_sw_scl, 1);
    sw_i2c_delay();
    gpio_set_level(s_sw_scl, 0);
    sw_i2c_delay();
    return data;
}

/* Probe: send 7-bit address + write bit, check ACK. */
static bool sw_i2c_probe(uint8_t addr7)
{
    sw_i2c_start();
    bool ack = sw_i2c_write_byte((uint8_t)(addr7 << 1));
    sw_i2c_stop();
    return ack;
}

/* Recover a stuck bus: pulse SCL 9x with SDA released, then STOP. */
static void sw_i2c_reset(void)
{
    gpio_set_level(s_sw_sda, 1);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(s_sw_scl, 1);
        sw_i2c_delay();
        gpio_set_level(s_sw_scl, 0);
        sw_i2c_delay();
    }
    sw_i2c_start();
    sw_i2c_stop();
}

/* ========================================================================
 * I2C register access (software I2C)
 * ======================================================================== */

static esp_err_t gt911_read_reg(gt911_dev_t *dev, uint16_t reg,
                                uint8_t *buf, uint8_t len)
{
    sw_i2c_start();
    if (!sw_i2c_write_byte((uint8_t)(dev->addr << 1) | 0)) goto err; /* write addr */
    if (!sw_i2c_write_byte((uint8_t)(reg >> 8))) goto err;
    if (!sw_i2c_write_byte((uint8_t)(reg & 0xFF))) goto err;

    sw_i2c_start();  /* repeated start */
    if (!sw_i2c_write_byte((uint8_t)(dev->addr << 1) | 1)) goto err; /* read addr */

    for (uint8_t i = 0; i < len; i++) {
        buf[i] = sw_i2c_read_byte(i < (len - 1));  /* ACK all but last */
    }
    sw_i2c_stop();
    return ESP_OK;

err:
    sw_i2c_stop();
    return ESP_FAIL;
}

static esp_err_t gt911_write_reg(gt911_dev_t *dev, uint16_t reg,
                                 const uint8_t *buf, uint8_t len)
{
    sw_i2c_start();
    if (!sw_i2c_write_byte((uint8_t)(dev->addr << 1) | 0)) goto err;
    if (!sw_i2c_write_byte((uint8_t)(reg >> 8))) goto err;
    if (!sw_i2c_write_byte((uint8_t)(reg & 0xFF))) goto err;

    for (uint8_t i = 0; i < len; i++) {
        if (!sw_i2c_write_byte(buf[i])) goto err;
    }
    sw_i2c_stop();
    return ESP_OK;

err:
    sw_i2c_stop();
    return ESP_FAIL;
}

/* ---- Interrupt handler (ISR → Semaphore → Task) ---------------------------- */

static void IRAM_ATTR gt911_isr_handler(void *arg)
{
    gt911_dev_t *dev = (gt911_dev_t *)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(dev->irq_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void gt911_irq_task(void *arg)
{
    gt911_dev_t *dev = (gt911_dev_t *)arg;
    gt911_touch_data_t data;
    uint32_t fail_count = 0;

    ESP_LOGI(TAG, "IRQ task started");

    /* Initial scan to clear any pending status after reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    sw_i2c_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    gt911_scan(dev, &data);
    ESP_LOGI(TAG, "Initial scan complete, waiting for touch...");

    while (1) {
        if (xSemaphoreTake(dev->irq_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {

            if (dev->suspended) continue;

            vTaskDelay(pdMS_TO_TICKS(5));

            esp_err_t err;
            for (int retry = 0; retry < 3; retry++) {
                err = gt911_scan(dev, &data);
                if (err == ESP_OK) break;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            if (err != ESP_OK) {
                fail_count++;
                ESP_LOGW(TAG, "I2C scan failed (%lu): %s",
                         fail_count, esp_err_to_name(err));
                if (fail_count >= 2) {
                    ESP_LOGW(TAG, "Resetting I2C bus...");
                    sw_i2c_reset();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    fail_count = 0;
                }
                continue;
            }
            fail_count = 0;

            if (dev->touch_cb) {
                dev->touch_cb(data.points, data.count, dev->cb_user_data);
            }
        }
    }
}

/* ---- Touch scan (ported from GT911_Scan in H7 reference) ---- */

esp_err_t gt911_scan(gt911_dev_t *dev, gt911_touch_data_t *data)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, TAG, "dev is NULL");

    uint8_t buf[41] = {0};   /* 5 points × 8 bytes + 1 status byte */
    uint8_t clear = 0;

    /* Read status register */
    esp_err_t err = gt911_read_reg(dev, GT911_READ_XY_REG, buf, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read status failed: %s", esp_err_to_name(err));
        return err;
    }

    if ((buf[0] & 0x80) == 0x00) {
        gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);
        if (data) { data->count = 0; }
        return ESP_OK;
    }

    uint8_t count = buf[0] & 0x0F;
    if (count > GT911_MAX_TOUCH_POINTS || count == 0) {
        gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);
        if (data) { data->count = 0; }
        return ESP_OK;
    }

    err = gt911_read_reg(dev, GT911_READ_XY_REG + 1, &buf[1], count * 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read touch points failed: %s", esp_err_to_name(err));
        return err;
    }

    gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);

    if (data) {
        data->count = count;
        for (uint8_t i = 0; i < count; i++) {
            data->points[i].track_id = buf[1 + (8 * i)];
            data->points[i].x        = ((uint16_t)buf[3 + (8 * i)] << 8) + buf[2 + (8 * i)];
            data->points[i].y        = ((uint16_t)buf[5 + (8 * i)] << 8) + buf[4 + (8 * i)];
            data->points[i].size     = ((uint16_t)buf[7 + (8 * i)] << 8) + buf[6 + (8 * i)];

            if (data->points[i].y < 20) data->points[i].y = 20;
            if (data->points[i].y > dev->max_height - 20)
                data->points[i].y = dev->max_height - 20;
            if (data->points[i].x < 20) data->points[i].x = 20;
            if (data->points[i].x > dev->max_width - 20)
                data->points[i].x = dev->max_width - 20;
        }
    }

    return ESP_OK;
}

/* ---- Firmware version ---- */

esp_err_t gt911_get_firmware_version(gt911_dev_t *dev, uint16_t *version)
{
    ESP_RETURN_ON_FALSE(dev && version, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    uint8_t buf[2] = {0};
    esp_err_t err = gt911_read_reg(dev, GT911_FIRMWARE_VERSION_REG, buf, 2);
    if (err == ESP_OK) {
        *version = ((uint16_t)buf[1] << 8) + buf[0];
    }
    return err;
}

/* ---- Init ---- */

esp_err_t gt911_init(const gt911_config_t *config, gt911_dev_t **out_dev)
{
    ESP_RETURN_ON_FALSE(config && out_dev, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    *out_dev = NULL;

    gt911_dev_t *dev = calloc(1, sizeof(gt911_dev_t));
    if (!dev) return ESP_ERR_NO_MEM;

    dev->rst_pin       = config->rst_pin;
    dev->int_pin       = config->int_pin;
    dev->max_width     = config->max_width  ? config->max_width  : GT911_MAX_WIDTH;
    dev->max_height    = config->max_height ? config->max_height : GT911_MAX_HEIGHT;
    dev->use_interrupt = config->use_interrupt;
    dev->irq_sem       = NULL;
    dev->irq_task      = NULL;
    dev->touch_cb      = NULL;
    dev->suspended     = false;

    /* Init software I2C pins (caller must do hardware reset before this) */
    ESP_LOGI(TAG, "Init software I2C: SDA=GPIO%d, SCL=GPIO%d",
             config->i2c_sda_pin, config->i2c_scl_pin);
    sw_i2c_init(config->i2c_scl_pin, config->i2c_sda_pin);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Probe GT911 at known addresses */
    const uint8_t addrs[] = { 0x5D, 0x14 };
    bool found = false;

    for (int i = 0; i < 2; i++) {
        ESP_LOGI(TAG, "Probing 0x%02X (software I2C)...", addrs[i]);
        if (sw_i2c_probe(addrs[i])) {
            ESP_LOGI(TAG, "  ACK at 0x%02X", addrs[i]);
            dev->addr = addrs[i];
            found = true;
            break;
        }
        ESP_LOGW(TAG, "  No ACK at 0x%02X", addrs[i]);
    }

    if (!found) {
        ESP_LOGE(TAG, "GT911 NOT FOUND on software I2C bus!");
        free(dev);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read product ID to verify */
    uint8_t pid[3] = {0};
    gt911_read_reg(dev, GT911_PRODUCT_ID_REG, pid, 3);
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t cfg_ver = 0;
    gt911_read_reg(dev, GT911_CONFIG_REG, &cfg_ver, 1);

    ESP_LOGI(TAG, "Product ID: %c,%c,%c  Config version: 0x%02X",
             pid[0], pid[1], pid[2], cfg_ver);

    if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
        uint16_t fw = 0;
        gt911_get_firmware_version(dev, &fw);
        ESP_LOGI(TAG, "Firmware version: 0x%04X", fw);
        ESP_LOGI(TAG, "GT911 found at 0x%02X", dev->addr);
    }

    *out_dev = dev;
    return ESP_OK;
}

/* ---- Deinit ---- */

esp_err_t gt911_deinit(gt911_dev_t *dev)
{
    if (!dev) return ESP_OK;

    if (dev->irq_task) {
        vTaskDelete(dev->irq_task);
        dev->irq_task = NULL;
    }
    if (dev->irq_sem) {
        vSemaphoreDelete(dev->irq_sem);
        dev->irq_sem = NULL;
    }
    if (dev->int_pin != GPIO_NUM_NC && dev->use_interrupt) {
        gpio_intr_disable(dev->int_pin);
        gpio_isr_handler_remove(dev->int_pin);
    }
    free(dev);
    return ESP_OK;
}

/* ---- Interrupt registration ---- */

esp_err_t gt911_register_isr(gt911_dev_t *dev, gt911_touch_cb_t cb,
                              void *user_data)
{
    ESP_RETURN_ON_FALSE(dev && cb, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(dev->use_interrupt, ESP_ERR_INVALID_STATE, TAG,
                        "interrupt mode not configured");

    dev->touch_cb      = cb;
    dev->cb_user_data  = user_data;
    dev->suspended     = false;

    dev->irq_sem = xSemaphoreCreateBinary();
    if (!dev->irq_sem) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreate(gt911_irq_task, "gt911_irq",
                                  3072, dev, 10, &dev->irq_task);
    if (ret != pdPASS) {
        vSemaphoreDelete(dev->irq_sem);
        dev->irq_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t int_conf = {
        .pin_bit_mask = BIT64(dev->int_pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_conf);

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(isr_err));
    }
    gpio_isr_handler_add(dev->int_pin, gt911_isr_handler, dev);

    ESP_LOGI(TAG, "Interrupt mode enabled (GPIO%d, negedge)", dev->int_pin);
    return ESP_OK;
}

/* ---- Suspend / Resume ---- */

esp_err_t gt911_suspend(gt911_dev_t *dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "NULL dev");
    if (dev->suspended) return ESP_OK;

    dev->suspended = true;

    if (dev->use_interrupt) {
        gpio_intr_disable(dev->int_pin);
    }

    ESP_LOGI(TAG, "Suspended");
    return ESP_OK;
}

esp_err_t gt911_resume(gt911_dev_t *dev)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "NULL dev");
    if (!dev->suspended) return ESP_OK;

    dev->suspended = false;

    if (dev->use_interrupt) {
        gpio_intr_enable(dev->int_pin);
    }

    ESP_LOGI(TAG, "Resumed");
    return ESP_OK;
}

/* ---- Quick touch check (polling, no ISR) ---- */

bool gt911_is_touched(gt911_dev_t *dev)
{
    if (!dev) return false;

    uint8_t status;
    if (gt911_read_reg(dev, GT911_READ_XY_REG, &status, 1) != ESP_OK)
        return false;

    return (status & 0x80) != 0;
}
