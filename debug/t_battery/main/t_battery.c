#include "nomadcast.h"
/* PCB V1.2 battery test: CHAG=GPIO8, BAT_M=GPIO3 / ADC1_CH2.
 * Presence/full indications remain voltage-based heuristics.
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

#define PIN_CHAG NOMADCAST_BAT_CHG_PIN
#define PIN_BAT_M NOMADCAST_PIN_BAT_ADC
#define BAT_ADC_UNIT NOMADCAST_BAT_ADC_UNIT
#define BAT_ADC_CHANNEL NOMADCAST_BAT_ADC_CHANNEL
#define BAT_DIVIDER NOMADCAST_BAT_DIVIDER
#define BAT_SAMPLES     16
#define BAT_PRESENT_MV  2800                 /* real V above this => battery present */
#define BAT_FULL_MV     4150                 /* not charging & >= this => full */



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
    ESP_LOGI(TAG, "=== t_battery: PCB V1.2 battery test ===");
    ESP_LOGI(TAG, "CHAG=GPIO%d (LOW=charging) BAT_M=GPIO%d ADC channel=%d (x%d divider)", PIN_CHAG, PIN_BAT_M, BAT_ADC_CHANNEL, BAT_DIVIDER);
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
