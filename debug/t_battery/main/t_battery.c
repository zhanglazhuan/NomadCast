/*
 * NomadCast — t_battery: battery presence / voltage / charge test (Leisound V1)
 *
 *   CHAG   = GPIO4   charge status (LOW=charging, HIGH=not) — input + pull-up
 *   BAT_M  = GPIO8   battery voltage sense → ADC1_CH7, x2 divider
 *
 * present/full are best-effort heuristics (only these two pins are available).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "t_battery";

#define PIN_CHAG        GPIO_NUM_4          /* charge status: LOW=charging */
#define PIN_BAT_M       GPIO_NUM_8          /* battery measure (ADC1_CH7), x2 divider */
#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_7        /* GPIO8 on ESP32-S3 */
#define BAT_DIVIDER     2                    /* on-board resistor divider */
#define BAT_SAMPLES     16
#define BAT_PRESENT_MV  2800                 /* real V above this => battery present */
#define BAT_FULL_MV     4150                 /* not charging & >= this => full */

/* If BT_M reads ~0V, the divider may be gated behind peripheral power EN_PWR
 * (GPIO46). To try it: add `#define PIN_EN_PWR GPIO_NUM_46`, configure it as
 * output and drive high in app_main before adc_init(). Off by default. */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BAT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali));
}

/* Averaged real battery voltage in millivolts (after x2 divider). 0 on failure. */
static int battery_voltage_mv(void)
{
    long sum = 0;
    int got = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        sum += mv;
        got++;
    }
    if (got == 0) return 0;
    return (int)(sum / got) * BAT_DIVIDER;
}

static bool battery_is_charging(void)
{
    return gpio_get_level(PIN_CHAG) == 0;   /* LOW = charging */
}

static bool battery_present(int mv)
{
    return mv > BAT_PRESENT_MV;
}

static bool battery_is_full(int mv, bool charging)
{
    return !charging && mv >= BAT_FULL_MV;
}

static void gpio_init(void)
{
    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT64(PIN_CHAG),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_battery: Leisound V1 battery test ===");
    ESP_LOGI(TAG, "CHAG=GPIO4 (LOW=charging)  BAT_M=GPIO8=ADC1_CH7 (x2 divider)");
    ESP_LOGI(TAG, "present/full are best-effort (only CHAG + BT_M available)");

    gpio_init();
    adc_init();

    while (1) {
        int  mv       = battery_voltage_mv();
        bool charging = battery_is_charging();
        bool present  = battery_present(mv);
        bool full     = battery_is_full(mv, charging);

        ESP_LOGI(TAG, "present=%-3s  V=%d.%02dV  charging=%-3s  full=%-3s",
                 present  ? "YES" : "NO",
                 mv / 1000, (mv % 1000) / 10,
                 charging ? "YES" : "NO",
                 full     ? "YES" : "NO");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
