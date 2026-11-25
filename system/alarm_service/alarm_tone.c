/*
 * NomadCast — Alarm ringtone (I2S sine beep)
 *
 * Drives the ES8156 DAC directly over the SAME I2S controller the audio
 * player uses (I2S_NUM_0), on the same pins (MCLK=GPIO13, BCLK=GPIO12,
 * WS=GPIO10, DOUT=GPIO11).  alarm_service_ring() tears down the audio player
 * (audio_player_stop) before ringing, and alarm_tone_stop() runs before
 * playback resumes, so the two are never active at the same time.
 *
 * Using a *second* controller (I2S_NUM_1) on those same physical pins was a
 * bug: two I2S controllers cannot own the same GPIO pins.  After the alarm
 * tone tore down I2S1 and revoked the pins, the next audio_player_play() on
 * I2S0 left the ESP32-S3 GPIO matrix / I2S clock state inconsistent and
 * crashed (reboot), which is exactly the "alarm rings → dismiss → play →
 * whole UI restarts" report.
 *
 * 880 Hz sine, 500 ms on / 500 ms off, until alarm_tone_stop().  ES8156 needs
 * a master clock, so MCLK (GPIO13) is driven at the default 256× sample-rate
 * ratio — the same arrangement audio_player uses.
 */

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "driver/i2s_std.h"

#include "nomadcast.h"

static const char *TAG = "alarm_tone";

#define SAMPLE_RATE    44100
#define TONE_FREQ      880
#define AMPLITUDE      12000
#define ON_MS          500
#define OFF_MS         500
#define TASK_STACK     4096

#define PERIOD_FRAMES  (SAMPLE_RATE / TONE_FREQ)   /* ~50 frames per sine cycle */

static i2s_chan_handle_t s_tx = NULL;
static volatile bool s_running = false;

static void tone_task(void *arg)
{
    (void)arg;

    int16_t on_buf[PERIOD_FRAMES * 2];
    int16_t off_buf[PERIOD_FRAMES * 2];
    for (int i = 0; i < PERIOD_FRAMES; i++) {
        int16_t s = (int16_t)(AMPLITUDE * sinf(2.0f * (float)M_PI * i / PERIOD_FRAMES));
        on_buf[i * 2]     = s;
        on_buf[i * 2 + 1] = s;
        off_buf[i * 2]     = 0;
        off_buf[i * 2 + 1] = 0;
    }

    /* Streaming the DMA buffer paces the loop in real time (~1.1 ms per write),
     * so esp_timer_get_time() gives clean ON/OFF phase boundaries. */
    while (s_running) {
        int64_t end = esp_timer_get_time() + ON_MS * 1000;
        while (s_running && esp_timer_get_time() < end) {
            size_t bw = 0;
            if (i2s_channel_write(s_tx, on_buf, sizeof(on_buf), &bw, pdMS_TO_TICKS(20)) != ESP_OK || bw == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));   /* never busy-spin on a failed write */
            }
        }

        end = esp_timer_get_time() + OFF_MS * 1000;
        while (s_running && esp_timer_get_time() < end) {
            size_t bw = 0;
            if (i2s_channel_write(s_tx, off_buf, sizeof(off_buf), &bw, pdMS_TO_TICKS(20)) != ESP_OK || bw == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }

    vTaskDelete(NULL);
}

void alarm_tone_start(void)
{
    if (s_running) return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        s_tx = NULL;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = NOMADCAST_PIN_I2S_MCLK,   /* ES8156 needs a master clock */
            .bclk = NOMADCAST_PIN_I2S_BCLK,
            .ws   = NOMADCAST_PIN_I2S_WS,
            .dout = NOMADCAST_PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }
    i2s_channel_enable(s_tx);

    s_running = true;
    /* Pin the tone task to CPU1 — the LVGL/UI loop runs on CPU0 (main task
     * affinity), so a high-priority audio task on CPU0 would starve it and
     * freeze the UI (this is exactly why audio_player pins its pipeline to
     * core 1). */
    if (xTaskCreatePinnedToCore(tone_task, "alarm_tone", TASK_STACK, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }

    ESP_LOGI(TAG, "ringing (%d Hz)", TONE_FREQ);
}

void alarm_tone_stop(void)
{
    if (!s_running && !s_tx) return;

    s_running = false;
    /* Let the tone task observe the flag and exit (it self-deletes). */
    vTaskDelay(pdMS_TO_TICKS(60));

    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    ESP_LOGI(TAG, "stopped");
}
