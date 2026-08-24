/*
 * NomadCast — Speaker + Earphone Plug Detection Test
 *
 * Starts playing an 800 Hz beep through the speaker. When earphones are
 * inserted (AMP_EN=GPIO43 goes HIGH), immediately disables the speaker amp
 * (AP_EN=GPIO21 → LOW) so audio routes through the earphone path.
 * Re-enables the speaker when earphones are removed.
 *
 * === Leisound V1 Pins ===
 *   EN_PWR   = GPIO46   全板外设电源
 *   AP_EN    = GPIO21   HT6872 功放使能 (HIGH=speaker on, LOW=earphone)
 *   AMP_EN   = GPIO43   耳机检测 (输入, HIGH=已插入)
 *   I2S BCLK = GPIO5
 *   I2S LRCLK= GPIO6
 *   I2S DOUT = GPIO7
 */

#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

static const char *TAG = "beep";

#define PIN_EN_POWER    GPIO_NUM_46
#define PIN_AP_EN       GPIO_NUM_44
#define PIN_AMP_EN      GPIO_NUM_11

#define PIN_I2S_MCLK    GPIO_NUM_1
#define PIN_I2S_BCLK    GPIO_NUM_2
#define PIN_I2S_LRCLK   GPIO_NUM_41
#define PIN_I2S_DOUT    GPIO_NUM_42

#define SAMPLE_RATE     44100
#define BEEP_FREQ       800
#define AMPLITUDE       8000

static i2s_chan_handle_t i2s_tx;

static void i2s_init(void)
{
    ESP_LOGI(TAG, "Init I2S0: BCLK=%d LRCLK=%d DOUT=%d rate=%d",
             PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT, SAMPLE_RATE);

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &i2s_tx, NULL));

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));

    /* Preload silence to start the I2S clock */
    int16_t sil[256] = {0};
    size_t bw;
    i2s_channel_write(i2s_tx, sil, sizeof(sil), &bw, portMAX_DELAY);
    ESP_LOGI(TAG, "I2S0 ready");
}

/* Hardware discovery: try every GPIO that could be a jack-detect pin.
 * After 5 seconds the speaker mutes (AP_EN=0) so you can check if the earphone
 * works when the amp is off — this proves the audio path is functional. */
static void hw_discovery(void *arg) {
    ESP_LOGW(TAG, "≈≈≈ SPEAKER OFF — is earphone playing? ≈≈≈");
    gpio_set_level(PIN_AP_EN, 0);  /* mute speaker amp */
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Speaker + Earphone Plug Test ===");

    /* Power */
    gpio_set_direction(PIN_EN_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN_POWER, 1);

    /* Speaker amp (GPIO21) — output, start ON */
    gpio_set_direction(PIN_AP_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AP_EN, 1);

    /* AMP_EN (GPIO43) — schematic shows it's HT6872 SHDN pin, NOT a jack-detect.
     * The earphone jack on this board likely has NO MCU-connected detection pin.
     * Read it as a data point, but don't expect it to change on plug/unplug. */
    gpio_set_direction(PIN_AMP_EN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_AMP_EN, GPIO_PULLDOWN_ONLY);

    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "HW: EN_POWER=1 AP_EN=1 AMP_EN=%d", gpio_get_level(PIN_AMP_EN));

    /* I2S */
    i2s_init();

    /* After 5s of speaker beep, mute the amp. Plug in earphones to verify
     * the DAC→headphone path works when the speaker amp is off. */
    const esp_timer_create_args_t timer_args = { .callback = hw_discovery };
    esp_timer_handle_t hp_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &hp_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(hp_timer, 5 * 1000 * 1000));

    /* Sine LUT */
    #define PERIOD_SAMPLES  (SAMPLE_RATE / BEEP_FREQ)
    int16_t buf[PERIOD_SAMPLES * 2];
    for (int i = 0; i < PERIOD_SAMPLES; i++) {
        int16_t s = (int16_t)(AMPLITUDE * sin(2.0 * M_PI * i / PERIOD_SAMPLES));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }

    ESP_LOGI(TAG, "=== Beeping %d Hz — insert earphone to switch ===", BEEP_FREQ);

    int loop = 0;
    while (1) {
        size_t bw;
        i2s_channel_write(i2s_tx, buf, sizeof(buf), &bw, portMAX_DELAY);
        if (++loop % 200 == 0) ESP_LOGI(TAG, "Beep loop %d, AMP_EN=%d AP_EN=%d",
                                        loop, gpio_get_level(PIN_AMP_EN),
                                        gpio_get_level(PIN_AP_EN));
    }
}
