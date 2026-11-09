/* PCB V1.2: ES7111 I2S DAC -> HT6872 speaker amplifier.
 * The ES8156 headphone DAC is not initialized by this speaker test. */
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "nomadcast.h"

static const char *TAG = "beep";
#define SAMPLE_RATE 44100
#define BEEP_FREQ 800
#define AMPLITUDE 8000
#define FRAMES 256
static i2s_chan_handle_t i2s_tx;

static void write_audio(const int16_t *buffer, size_t size)
{
    size_t written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(i2s_tx, buffer, size, &written, 1000));
    ESP_ERROR_CHECK(written == size ? ESP_OK : ESP_FAIL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Speaker test / PCB V1.2 / ES7111 + HT6872 ===");
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << NOMADCAST_PIN_AP_POWER) | (1ULL << NOMADCAST_PIN_AMP_EN),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_AMP_EN, 0));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_AP_POWER, 1));
    gpio_config_t detect = {
        .pin_bit_mask = 1ULL << NOMADCAST_PIN_HP_DETECT,
        .mode = GPIO_MODE_INPUT,
        /* Use the external jack detection resistor network. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&detect));
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "I2S0: BCLK=%d LRCLK=%d DOUT=%d rate=%d; ES7111 needs no MCLK",
             NOMADCAST_PIN_I2S_BCLK, NOMADCAST_PIN_I2S_WS,
             NOMADCAST_PIN_I2S_DOUT, SAMPLE_RATE);
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &i2s_tx, NULL));
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = NOMADCAST_PIN_I2S_BCLK,
            .ws = NOMADCAST_PIN_I2S_WS,
            .dout = NOMADCAST_PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &config));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    int16_t buffer[FRAMES * 2] = {0};
    write_audio(buffer, sizeof(buffer));
    int plugged = gpio_get_level(NOMADCAST_PIN_HP_DETECT);
    int candidate = plugged;
    int64_t candidate_since = esp_timer_get_time();
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_AMP_EN, !plugged));
    ESP_LOGI(TAG, "800 Hz continuous tone; jack GPIO%d HIGH mutes speaker. Headphone DAC not initialized.",
             NOMADCAST_PIN_HP_DETECT);
    double phase = 0;
    int64_t next_status = 0;
    uint32_t blocks = 0;
    while (1) {
        for (int i = 0; i < FRAMES; ++i) {
            int16_t sample = (int16_t)(AMPLITUDE * sin(phase));
            buffer[2 * i] = buffer[2 * i + 1] = sample;
            phase += 2.0 * M_PI * BEEP_FREQ / SAMPLE_RATE;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        write_audio(buffer, sizeof(buffer));
        ++blocks;
        int64_t now = esp_timer_get_time();
        int level = gpio_get_level(NOMADCAST_PIN_HP_DETECT);
        if (level != candidate) {
            candidate = level;
            candidate_since = now;
        }
        if (candidate != plugged && now - candidate_since >= 50000) {
            plugged = candidate;
            ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_AMP_EN, !plugged));
            ESP_LOGI(TAG, "Jack %s; speaker %s", plugged ? "inserted" : "removed", plugged ? "OFF" : "ON");
        }
        if (now >= next_status) {
            ESP_LOGI(TAG, "TX blocks=%lu POWER(GPIO%d)=%d AP_EN(GPIO%d)=%d HP_DETECT(GPIO%d)=%d",
                     (unsigned long)blocks, NOMADCAST_PIN_AP_POWER, gpio_get_level(NOMADCAST_PIN_AP_POWER),
                     NOMADCAST_PIN_AMP_EN, gpio_get_level(NOMADCAST_PIN_AMP_EN),
                     NOMADCAST_PIN_HP_DETECT, level);
            next_status = now + 5000000;
        }
    }
}
