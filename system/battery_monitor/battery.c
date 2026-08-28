/*
 * NomadCast — sys/battery: periodic battery monitor.
 *
 * Every 5 min: read voltage (ADC1_CH7 / GPIO8, x2 divider), quantize to percent
 * via a Li-ion OCV table, read charging (CHAG / GPIO4 low). When the status-bar
 * icon bucket or charging state changes, fire APP_EVENT_BATTERY_CHANGED.
 * ADC logic ported from debug/t_battery.
 */
#include "battery.h"
#include "app_event.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nomadcast_v1.h"

static const char *TAG = "battery";

#define PIN_CHAG        NOMADCAST_BAT_CHG_PIN
#define BAT_ADC_UNIT    NOMADCAST_BAT_ADC_UNIT
#define BAT_ADC_CHANNEL NOMADCAST_BAT_ADC_CHANNEL   /* GPIO8 on ESP32-S3 */
#define BAT_DIVIDER     NOMADCAST_BAT_DIVIDER
#define BAT_SAMPLES     16
#define BAT_PERIOD_MS   300000             /* 5 minutes */

typedef struct {
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t cali;
    lv_timer_t *timer;
    int last_bucket;
    int last_percent;
    bool last_charging;
} battery_state_t;

static battery_state_t s_battery = {
    .last_bucket = -1, /* First sample always fires. */
    .last_percent = -1,
};

/* Li-ion open-circuit voltage (real mV after x2 divider) → percent. */
static int battery_percent_from_mv(int mv)
{
    static const struct { int mv; int pct; } TBL[] = {
        {4200, 100}, {4000, 85}, {3900, 75}, {3800, 60},
        {3700, 40},  {3600, 20}, {3500, 10}, {3300, 0},
    };
    const int N = (int)(sizeof(TBL) / sizeof(TBL[0]));
    if (mv >= TBL[0].mv)     return 100;
    if (mv <= TBL[N - 1].mv) return 0;
    for (int i = 0; i < N - 1; i++) {
        if (mv <= TBL[i].mv && mv >= TBL[i + 1].mv) {
            int dv = TBL[i].mv  - TBL[i + 1].mv;
            int dp = TBL[i].pct - TBL[i + 1].pct;
            return TBL[i + 1].pct + (mv - TBL[i + 1].mv) * dp / dv;
        }
    }
    return 0;
}

/* Mirror lv_status_bar get_battery_icon() thresholds so the event fires exactly
 * when the displayed icon would change. */
static int battery_bucket(int pct)
{
    if (pct >= 100) return 7;
    if (pct >= 86)  return 6;
    if (pct >= 72)  return 5;
    if (pct >= 58)  return 4;
    if (pct >= 43)  return 3;
    if (pct >= 29)  return 2;
    if (pct >= 15)  return 1;
    return 0;
}

/* Averaged real battery voltage in millivolts (after x2 divider). 0 on failure. */
static int battery_voltage_mv(void)
{
    long sum = 0;
    int got = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_battery.adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (adc_cali_raw_to_voltage(s_battery.cali, raw, &mv) != ESP_OK) continue;
        sum += mv;
        got++;
    }
    if (got == 0) return 0;
    return (int)(sum / got) * BAT_DIVIDER;
}

static void battery_sample(void)
{
    int  mv       = battery_voltage_mv();
    int  pct      = battery_percent_from_mv(mv);
    bool charging = (gpio_get_level(PIN_CHAG) == 0);
    int  bucket   = battery_bucket(pct);

    bool changed = (bucket != s_battery.last_bucket) || (charging != s_battery.last_charging);

    s_battery.last_percent  = pct;
    s_battery.last_charging = charging;
    s_battery.last_bucket   = bucket;

    if (changed) {
        app_event_battery_t data = { .percent = pct, .charging = charging };
        app_event_fire(APP_EVENT_BATTERY_CHANGED, &data);
        ESP_LOGI(TAG, "%d%% charging=%d (mv=%d) -> event", pct, charging, mv);
    }
}

static void battery_timer_cb(lv_timer_t *t)
{
    (void)t;
    battery_sample();
}

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_battery.adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_battery.adc, BAT_ADC_CHANNEL, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BAT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery.cali));

    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT64(PIN_CHAG),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    battery_sample();                       /* immediate — initialize the icon */
    s_battery.timer = lv_timer_create(battery_timer_cb, BAT_PERIOD_MS, NULL);
    lv_timer_set_repeat_count(s_battery.timer, -1);

    ESP_LOGI(TAG, "Initialized (5-min sampler, CHAG=GPIO%d, BT_M=ADC1_CH%d)",
             (int)NOMADCAST_BAT_CHG_PIN, (int)NOMADCAST_BAT_ADC_CHANNEL);
}

int  battery_get_percent(void) { return s_battery.last_percent; }
bool battery_is_charging(void) { return s_battery.last_charging; }
