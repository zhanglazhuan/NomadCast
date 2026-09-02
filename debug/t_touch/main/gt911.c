/*
 * GT911 Capacitive Touch Controller Driver — Implementation
 *
 * Ported from H7_GT911_I2C_Touch-master (STM32H7) to ESP-IDF.
 * Reference hardware: Leisound V1 (ESP32-S3 + GT911).
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "gt911.h"

static const char *TAG = "gt911";

/* ---- Device handle (opaque) ---- */

struct gt911_dev_t {
    i2c_master_bus_handle_t   i2c_bus;
    i2c_master_dev_handle_t   i2c_dev;
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

/* Forward declaration */
static esp_err_t gt911_hw_reset(const gt911_config_t *cfg);

/* ---- I2C register access ---- */

static esp_err_t gt911_read_reg(gt911_dev_t *dev, uint16_t reg,
                                uint8_t *buf, uint8_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(dev->i2c_dev, reg_buf, 2,
                                       buf, len, pdMS_TO_TICKS(200));
}

static esp_err_t gt911_write_reg(gt911_dev_t *dev, uint16_t reg,
                                 const uint8_t *buf, uint8_t len)
{
    uint8_t *wb = malloc(2 + len);
    if (!wb) return ESP_ERR_NO_MEM;

    wb[0] = (uint8_t)(reg >> 8);
    wb[1] = (uint8_t)(reg & 0xFF);
    memcpy(wb + 2, buf, len);
    esp_err_t ret = i2c_master_transmit(dev->i2c_dev, wb, 2 + len,
                                        pdMS_TO_TICKS(200));
    free(wb);
    return ret;
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

    /* Initial scan to clear any pending status after reset.
     * GT911 pulls INT LOW after reset to indicate readiness;
     * we must read & clear the status register so INT goes HIGH.
     * Then subsequent touches will generate proper NEGEDGE interrupts.
     * Use a longer delay — system may still be initializing LCD/WiFi/etc. */
    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_master_bus_reset(dev->i2c_bus);
    vTaskDelay(pdMS_TO_TICKS(50));
    gt911_scan(dev, &data);
    ESP_LOGI(TAG, "Initial scan complete, waiting for touch...");

    while (1) {
        /* Wait for INT signal (with timeout for health check) */
        if (xSemaphoreTake(dev->irq_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {

            if (dev->suspended) continue;

            /* Small delay to let I2C bus and GT911 settle after INT */
            vTaskDelay(pdMS_TO_TICKS(5));

            /* Scan touch data (I2C, must be done in task context) */
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
                    /* I2C bus recovery: reset the bus */
                    ESP_LOGW(TAG, "Resetting I2C bus...");
                    i2c_master_bus_reset(dev->i2c_bus);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    fail_count = 0;
                }
                continue;
            }
            fail_count = 0;

            /* Notify callback */
            if (dev->touch_cb) {
                dev->touch_cb(data.points, data.count, dev->cb_user_data);
            }
        }
        /* Timeout: no touch for 5s — normal, just loop */
    }
}

/* ---- Hardware reset (from H7 reference: RST+INT LOW → RST HI → INT HI) ---- */

static esp_err_t gt911_hw_reset(const gt911_config_t *cfg)
{
    ESP_LOGI(TAG, "Performing hardware reset...");

    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(cfg->rst_pin) | BIT64(cfg->int_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    /* Step 1: RST LOW, INT LOW — 200ms */
    gpio_set_level(cfg->rst_pin, 0);
    gpio_set_level(cfg->int_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 2: RST HIGH — 200ms */
    gpio_set_level(cfg->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 3: INT HIGH — then reconfigure as input */
    gpio_set_level(cfg->int_pin, 1);

    gpio_config_t int_in_cfg = {
        .pin_bit_mask = BIT64(cfg->int_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_in_cfg);

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Reset complete, INT level = %d", gpio_get_level(cfg->int_pin));
    return ESP_OK;
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
        /* No touch data ready — clear status */
        gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);
        if (data) { data->count = 0; }
        return ESP_OK;
    }

    /* Touch data ready */
    uint8_t count = buf[0] & 0x0F;
    if (count > GT911_MAX_TOUCH_POINTS || count == 0) {
        gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);
        if (data) { data->count = 0; }
        return ESP_OK;
    }

    /* Read all touch point data */
    err = gt911_read_reg(dev, GT911_READ_XY_REG + 1, &buf[1], count * 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read touch points failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Always clear status after reading */
    gt911_write_reg(dev, GT911_READ_XY_REG, &clear, 1);

    if (data) {
        data->count = count;
        for (uint8_t i = 0; i < count; i++) {
            data->points[i].track_id = buf[1 + (8 * i)];
            data->points[i].x        = ((uint16_t)buf[3 + (8 * i)] << 8) + buf[2 + (8 * i)];
            data->points[i].y        = ((uint16_t)buf[5 + (8 * i)] << 8) + buf[4 + (8 * i)];
            data->points[i].size     = ((uint16_t)buf[7 + (8 * i)] << 8) + buf[6 + (8 * i)];

            /* Clamp coordinates to display bounds */
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

    /* Allocate device handle */
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

    /* Step 1: I2C bus init (caller must do hardware reset before this) */
    ESP_LOGI(TAG, "Init I2C: SDA=GPIO%d, SCL=GPIO%d, freq=%lu Hz",
             config->i2c_sda_pin, config->i2c_scl_pin,
             config->i2c_freq_hz ? config->i2c_freq_hz : 100000);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port         = I2C_NUM_0,
        .sda_io_num       = config->i2c_sda_pin,
        .scl_io_num       = config->i2c_scl_pin,
        .clk_source       = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &dev->i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 3: Probe GT911 */
    const uint8_t addrs[] = { GT911_I2C_ADDR, GT911_I2C_ADDR_ALT };
    bool found = false;

    for (int i = 0; i < 2; i++) {
        uint8_t raw_addr = addrs[i];
        uint8_t probe_addr = (raw_addr == 0xBA) ? 0x5D : raw_addr;

        ESP_LOGI(TAG, "Probing 0x%02X (raw 0x%02X)...", probe_addr, raw_addr);

        if (i2c_master_probe(dev->i2c_bus, probe_addr,
                             pdMS_TO_TICKS(200)) != ESP_OK) {
            ESP_LOGW(TAG, "  No ACK at 0x%02X", probe_addr);
            continue;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = probe_addr,
            .scl_speed_hz    = config->i2c_freq_hz ? config->i2c_freq_hz : 100000,
        };
        err = i2c_master_bus_add_device(dev->i2c_bus, &dev_cfg, &dev->i2c_dev);
        if (err != ESP_OK) continue;

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
            ESP_LOGI(TAG, "GT911 found at 0x%02X", probe_addr);
            found = true;
            break;
        }

        i2c_master_bus_rm_device(dev->i2c_dev);
        dev->i2c_dev = NULL;
    }

    if (!found) {
        ESP_LOGE(TAG, "GT911 NOT FOUND on I2C bus!");
        if (dev->i2c_dev) i2c_master_bus_rm_device(dev->i2c_dev);
        if (dev->i2c_bus)  i2c_del_master_bus(dev->i2c_bus);
        free(dev);
        return ESP_ERR_NOT_FOUND;
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
    if (dev->i2c_dev) i2c_master_bus_rm_device(dev->i2c_dev);
    if (dev->i2c_bus) i2c_del_master_bus(dev->i2c_bus);
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

    /* Create binary semaphore for ISR→Task signalling */
    dev->irq_sem = xSemaphoreCreateBinary();
    if (!dev->irq_sem) return ESP_ERR_NO_MEM;

    /* Create task for I2C touch scanning (stack: 3KB, priority: high) */
    BaseType_t ret = xTaskCreate(gt911_irq_task, "gt911_irq",
                                  3072, dev, 10, &dev->irq_task);
    if (ret != pdPASS) {
        vSemaphoreDelete(dev->irq_sem);
        dev->irq_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Configure INT pin for falling-edge interrupt
     * (GT911 pulls INT LOW when data is ready) */
    gpio_config_t int_conf = {
        .pin_bit_mask = BIT64(dev->int_pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,  /* falling edge = touch event */
    };
    gpio_config(&int_conf);

    /* Install ISR (service may already be installed by other drivers) */
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

    /* Disable INT pin interrupt */
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

    /* Re-enable INT pin interrupt */
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
