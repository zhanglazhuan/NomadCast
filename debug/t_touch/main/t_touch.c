/*
 * Leisound V1 — GT911 Interrupt Mode Test
 *
 * Uses the project's drivers/gt911 driver with use_interrupt=true.
 *
 * Data flow:
 *   GT911 INT pin (GPIO18) falling edge → GPIO ISR → semaphore
 *   → gt911_irq_task (FreeRTOS) → I2C scan → user callback → log
 *
 * Pins (Leisound V1):
 *   EN_POWER  GPIO46
 *   I2C SDA   GPIO47
 *   I2C SCL   GPIO48
 *   TP INT    GPIO18
 *   TP RST    GPIO8
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "gt911.h"

static const char *TAG = "t_touch";

/* ---- Pin map (Leisound V1) ---- */
#define PIN_EN_POWER   GPIO_NUM_43
#define PIN_I2C_SDA    GPIO_NUM_38
#define PIN_I2C_SCL    GPIO_NUM_45
#define PIN_TP_INT     GPIO_NUM_39
#define PIN_TP_RST     GPIO_NUM_40
#define LCD_W          240
#define LCD_H          320

static gt911_dev_t *s_gt911_dev = NULL;
static uint32_t     s_touch_count = 0;

/* ---- ISR callback (called from gt911_irq_task context, NOT ISR) ---- */

static void on_touch_event(const gt911_touch_point_t *points,
                           uint8_t count, void *user_data)
{
    (void)user_data;
    s_touch_count++;

    if (count > 0) {
        ESP_LOGI(TAG, "[%lu] TOUCH %d pt(s):", s_touch_count, count);
        for (uint8_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "  pt%d: x=%-4d y=%-4d size=%-4d id=%d",
                     i, points[i].x, points[i].y,
                     points[i].size, points[i].track_id);
        }
    } else {
        ESP_LOGI(TAG, "[%lu] RELEASE", s_touch_count);
    }
}

/* ---- Periodic status dump ---- */

static void status_timer_cb(void *arg)
{
    (void)arg;
    if (!s_gt911_dev) return;

    int int_level = gpio_get_level(PIN_TP_INT);
    bool touched = gt911_is_touched(s_gt911_dev);
    ESP_LOGI(TAG, "STATUS: INT_GPIO=%d touched=%d events=%lu",
             int_level, touched, s_touch_count);
}

/* ---- Main ---- */

void app_main(void)
{
    ESP_LOGI(TAG, "===== GT911 Interrupt Mode Test =====");

    /* 1. Power on */
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(PIN_EN_POWER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);
    ESP_LOGI(TAG, "Power ON");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 2. HW reset — RST+INT shared on GPIO8, INT also on GPIO18 */
    {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = BIT64(PIN_TP_RST) | BIT64(PIN_TP_INT),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(PIN_TP_INT, 0);
        gpio_set_level(PIN_TP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(PIN_TP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(PIN_TP_INT, 1);

        gpio_config_t int_in = {
            .pin_bit_mask = BIT64(PIN_TP_INT),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&int_in);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "Reset done, INT=%d", gpio_get_level(PIN_TP_INT));
    }

    /* 3. GT911 init with interrupt mode ON */
    gt911_config_t cfg = {
        .rst_pin       = PIN_TP_RST,
        .int_pin       = PIN_TP_INT,
        .i2c_sda_pin   = PIN_I2C_SDA,
        .i2c_scl_pin   = PIN_I2C_SCL,
        .i2c_freq_hz   = 100000,
        .max_width     = LCD_W,
        .max_height    = LCD_H,
        .use_interrupt = true,   /* <--- KEY: interrupt mode */
    };

    /* Software first-aid: force internal pull-ups on the I2C pins.
     * GPIO45 (strapping pin) may carry a default pull-down that fights the
     * I2C driver's internal pull-up — see if an explicit pull-up wins. */
    gpio_set_pull_mode(PIN_I2C_SCL, GPIO_PULLUP_ONLY);   /* GPIO45 */
    gpio_set_pull_mode(PIN_I2C_SDA, GPIO_PULLUP_ONLY);   /* GPIO38 */
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t err = gt911_init(&cfg, &s_gt911_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init FAILED: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* 4. Read firmware version */
    uint16_t fw = 0;
    gt911_get_firmware_version(s_gt911_dev, &fw);
    ESP_LOGI(TAG, "Firmware: 0x%04X", fw);

    /* 5. Register interrupt callback */
    err = gt911_register_isr(s_gt911_dev, on_touch_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISR register FAILED: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Interrupt mode ACTIVE — touch the screen!");

    /* 6. Periodic status dump every 5 seconds */
    esp_timer_handle_t status_timer = NULL;
    const esp_timer_create_args_t timer_args = {
        .callback = status_timer_cb,
        .name = "status",
    };
    esp_timer_create(&timer_args, &status_timer);
    esp_timer_start_periodic(status_timer, 5000000);  /* 5s */

    /* 7. Sleep — everything happens in gt911_irq_task */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
