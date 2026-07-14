/*
 * NomadCast Audio Player — ESP-ADF esp_audio wrapper.
 *
 * esp_audio manages: reader (fatfs "file://" / http by URI scheme) -> esp_decoder
 * (MP3/AAC/M4A/TS) -> i2s. Play/stop build/destroy the instance so internal DRAM
 * is freed when idle. Pause captures the byte position and tears down; resume
 * rebuilds and plays from that byte offset. Volume via ES8156 DAC over I2C.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "esp_crt_bundle.h"

#include "audio_element.h"
#include "audio_common.h"
#include "esp_audio.h"
#include "http_stream.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "esp_decoder.h"

#include "audio_player.h"
#include "driver/i2c_master.h"
#include "es8156.h"
#include "app_event.h"
#include "flash_store.h"

static const char *TAG = "audio_player";

/* ── I2S pins (from leisound_v1.h) ─────────────────────────────────── */
#define I2S_BCLK  GPIO_NUM_5
#define I2S_LRCLK GPIO_NUM_6
#define I2S_DOUT  GPIO_NUM_7
#define PIN_AP_EN GPIO_NUM_21  /* Amp enable */

/* i2s_stream calls get_i2s_pins() (CONFIG_AUDIO_BOARD_CUSTOM). */
#include "board_pins_config.h"
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *cfg) {
    (void)port;
    cfg->bck_io_num   = I2S_BCLK;
    cfg->ws_io_num    = I2S_LRCLK;
    cfg->data_out_num = I2S_DOUT;
    cfg->data_in_num  = I2S_GPIO_UNUSED;
    cfg->mck_io_num   = I2S_GPIO_UNUSED;
    return ESP_OK;
}

/* ── State ───────────────────────────────────────────────────────────── */
static esp_audio_handle_t s_esp     = NULL;
static bool  s_inited        = false;
static bool  s_playing       = false;
static int   s_resume_sec    = 0;    /* time position captured on pause */
static int   s_pending_seek  = -1;   /* pending seek target seconds (-1=none) */
static int   s_last_time_sec = 0;    /* last known play position (paused UI / save) */
static char  s_uri[2600];            /* last URI played (esp_audio form) */

static es8156_handle_t s_es8156 = NULL;
static int s_volume = 70;

/* ── Decoder (esp_decoder auto-detect MP3/AAC/M4A/TS) ────────────────── */
static audio_element_handle_t create_decoder(void) {
    esp_decoder_cfg_t cfg = DEFAULT_ESP_DECODER_CONFIG();
    cfg.task_core = 1;         /* pin to CPU1 — off the LVGL core */
    cfg.stack_in_ext = true;   /* decoder stack in PSRAM */
    cfg.plus_enable = true;    /* HE-AAC v1/v2 */
    audio_decoder_t decoder_list[] = {
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_M4A_DECODER_CONFIG(),
        DEFAULT_ESP_TS_DECODER_CONFIG(),
    };
    return esp_decoder_init(&cfg, decoder_list,
                            sizeof(decoder_list) / sizeof(decoder_list[0]));
}

/* ── esp_audio status callback → playing flag ────────────────────────── */
static void on_esp_audio_event(esp_audio_state_t *st, void *ctx) {
    (void)ctx;
    if (!st) return;
    if (st->status == AUDIO_STATUS_RUNNING && s_pending_seek >= 0) {
        esp_audio_seek(s_esp, s_pending_seek);
        s_pending_seek = -1;
    }
    if (st->status == AUDIO_STATUS_FINISHED ||
        st->status == AUDIO_STATUS_STOPPED  ||
        st->status == AUDIO_STATUS_ERROR) {
        s_playing = false;
    }
}

/* ── Build / teardown the esp_audio instance ─────────────────────────── */
static esp_audio_handle_t esp_audio_build(void) {
    esp_audio_cfg_t cfg = DEFAULT_ESP_AUDIO_CONFIG();
    cfg.cb_func     = on_esp_audio_event;
    cfg.prefer_type = ESP_AUDIO_PREFER_MEM;
    esp_audio_handle_t h = esp_audio_create(&cfg);
    if (!h) { ESP_LOGE(TAG, "esp_audio_create failed"); return NULL; }

    fatfs_stream_cfg_t fs = FATFS_STREAM_CFG_DEFAULT();
    fs.type = AUDIO_STREAM_READER;
    esp_audio_input_stream_add(h, fatfs_stream_init(&fs));

    http_stream_cfg_t http = HTTP_STREAM_CFG_DEFAULT();
    http.type = AUDIO_STREAM_READER;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    esp_audio_input_stream_add(h, http_stream_init(&http));

    esp_audio_codec_lib_add(h, AUDIO_CODEC_TYPE_DECODER, create_decoder());

    i2s_stream_cfg_t i2s = I2S_STREAM_CFG_DEFAULT();
    i2s.type = AUDIO_STREAM_WRITER;
    i2s.std_cfg.gpio_cfg.bclk = I2S_BCLK;
    i2s.std_cfg.gpio_cfg.ws   = I2S_LRCLK;
    i2s.std_cfg.gpio_cfg.dout = I2S_DOUT;
    i2s.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    i2s.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    esp_audio_output_stream_add(h, i2s_stream_init(&i2s));

    return h;
}

static void esp_audio_teardown(void) {
    if (s_esp) {
        esp_audio_stop(s_esp, TERMINATION_TYPE_NOW);
        esp_audio_destroy(s_esp);
        s_esp = NULL;
    }
    s_playing = false;
}

/* Local "/sdcard/X" -> "file://sdcard/X"; http(s) verbatim. */
static void to_esp_uri(const char *in, char *out, int out_sz) {
    if (strncmp(in, "http", 4) == 0)      snprintf(out, out_sz, "%s", in);
    else if (in[0] == '/')                snprintf(out, out_sz, "file://%s", in + 1);
    else                                  snprintf(out, out_sz, "%s", in);
}

/* ── Public API ──────────────────────────────────────────────────────── */
bool audio_player_init(void) {
    if (s_inited) return true;
    gpio_config_t out = { .pin_bit_mask = BIT64(PIN_AP_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(PIN_AP_EN, 1);
    s_inited = true;
    ESP_LOGI(TAG, "amp enabled (HT6872)");
    return true;
}

bool audio_player_play(const char *url) {
    if (!url || !url[0]) return false;
    esp_audio_teardown();
    s_resume_sec = 0;
    s_pending_seek = -1;
    s_last_time_sec = 0;
    to_esp_uri(url, s_uri, sizeof(s_uri));

    s_esp = esp_audio_build();
    if (!s_esp) return false;

    esp_audio_play(s_esp, AUDIO_CODEC_TYPE_DECODER, s_uri, 0);
    s_playing = true;
    ESP_LOGI(TAG, "play: %s", s_uri);
    return true;
}

void audio_player_stop(void) {
    if (s_esp) {
        int us = 0;
        if (esp_audio_time_get(s_esp, &us) == ESP_ERR_AUDIO_NO_ERROR) s_last_time_sec = us / 1000000;
    }
    esp_audio_teardown();
    s_resume_sec = 0;
    s_pending_seek = -1;
}

void audio_player_pause(bool pause) {
    if (pause) {
        if (!s_esp) return;
        s_resume_sec = 0;
        s_last_time_sec = 0;
        int us = 0;
        if (esp_audio_time_get(s_esp, &us) == ESP_ERR_AUDIO_NO_ERROR) {
            s_resume_sec = us / 1000000;
            s_last_time_sec = s_resume_sec;
        }
        esp_audio_teardown();                 /* frees DRAM -> is_active()=false */
        ESP_LOGI(TAG, "pause @ %ds", s_resume_sec);
    } else {
        if (!s_uri[0]) return;
        s_esp = esp_audio_build();
        if (!s_esp) return;
        s_pending_seek = s_resume_sec;
        esp_audio_play(s_esp, AUDIO_CODEC_TYPE_DECODER, s_uri, 0);  /* play from 0 — type detection works */
        s_playing = true;
        ESP_LOGI(TAG, "resume (seek %ds pending)", s_resume_sec);
    }
}

void audio_player_set_volume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    int reg = 0x77 + vol * (0xBF - 0x77) / 100;   /* ~ -40dB (0%) .. 0dB (100%) */
    if (s_es8156) es8156_write_reg(s_es8156, 0x14, (uint8_t)reg);
    flash_set_i32("settings", "volume", vol);
    ESP_LOGI(TAG, "volume %d%% (reg 0x%02X)", vol, reg);
}

int audio_player_get_volume(void) { return s_volume; }

static void on_key_volume_event(app_event_t event, const void *data) {
    (void)data;
    if (event == APP_EVENT_KEY_VOL_UP)        audio_player_set_volume(s_volume + 10);
    else if (event == APP_EVENT_KEY_VOL_DOWN) audio_player_set_volume(s_volume - 10);
}

void audio_player_codec_init(i2c_master_bus_handle_t bus) {
    if (bus) {
        es8156_config_t cfg = { .i2c_bus = bus, .i2c_address = 0x08 };
        if (es8156_initialize(&cfg, &s_es8156) != ESP_OK) {
            ESP_LOGW(TAG, "ES8156 init failed — volume control disabled");
            s_es8156 = NULL;
        }
    }
    s_volume = flash_get_i32("settings", "volume", 70);
    audio_player_set_volume(s_volume);        /* clamp + apply + persist */
    app_event_register(on_key_volume_event);
    ESP_LOGI(TAG, "codec init (volume %d%%, es8156=%p)", s_volume, (void *)s_es8156);
}

bool audio_player_is_playing(void) { return s_playing; }

int audio_player_get_position_sec(void) {
    if (s_esp && s_playing) {
        int us = 0;
        if (esp_audio_time_get(s_esp, &us) == ESP_ERR_AUDIO_NO_ERROR) s_last_time_sec = us / 1000000;
    }
    return s_last_time_sec;
}

bool audio_player_is_active(void) { return s_esp != NULL; }
