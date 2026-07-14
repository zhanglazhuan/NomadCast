/*
 * NomadCast Audio Player — manual audio_pipeline (no esp_audio .a)
 *
 * Pipeline:  reader (fatfs or http) → esp_decoder → softvol → i2s_stream
 *
 * Volume:    ES8156 I2C register (headphone) + software PCM scale (ES7111 speaker)
 *
 * Lifecycle: play()  = build → run
 *            stop()  = stop → deinit
 *            pause() = teardown  (saves position)
 *            resume()= rebuild   (seeks to saved position)
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "esp_crt_bundle.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_common.h"
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

/* ── I2S pins ─────────────────────────────────────────────────────────── */
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

/* ══════════════════════════════════════════════════════════════════════════
 * State
 * ══════════════════════════════════════════════════════════════════════════ */

static audio_pipeline_handle_t  s_pipeline      = NULL;
static audio_element_handle_t   s_i2s_el        = NULL;
static audio_element_handle_t   s_softvol_el     = NULL;

static bool  s_inited        = false;
static bool  s_playing       = false;
static bool  s_released      = false; /* pipeline torn down, position saved for resume */
static int   s_resume_sec    = 0;     /* saved time position (UI display) */
static int64_t s_resume_byte = 0;     /* decoder input byte offset (precise seek) */
static int   s_last_time_sec = 0;     /* last known play position */
static char  s_uri[2600];            /* last URI played */
static char  s_esp_uri[2600];        /* "file://sdcard/…" or "https://…" */

static es8156_handle_t s_es8156 = NULL;
static int s_volume = 70;

/* ══════════════════════════════════════════════════════════════════════════
 * Software volume element — sits between decoder and I2S
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t swvol_open(audio_element_handle_t self)   { (void)self; return ESP_OK; }
static esp_err_t swvol_close(audio_element_handle_t self)  { (void)self; return ESP_OK; }
static esp_err_t swvol_destroy(audio_element_handle_t self){ (void)self; return ESP_OK; }

static audio_element_err_t swvol_process(audio_element_handle_t self,
                                          char *buf, int buf_sz)
{
    int len = audio_element_input(self, buf, buf_sz);
    if (len <= 0) return len;

    int vol = s_volume;   /* shared, 0-200 */
    if (vol <= 0)   { memset(buf, 0, len); return audio_element_output(self, buf, len); }
    if (vol == 100) return audio_element_output(self, buf, len);  /* passthrough */

    int16_t *s = (int16_t *)buf;
    int count = len / 2;
    if (vol < 100) {
        /* Attenuation */
        for (int i = 0; i < count; i++) {
            s[i] = (int16_t)((int32_t)s[i] * vol / 100);
        }
    } else {
        /* Gain > 100% — amplify with soft-clip to prevent wrap-around */
        for (int i = 0; i < count; i++) {
            int32_t v = (int32_t)s[i] * vol / 100;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s[i] = (int16_t)v;
        }
    }
    return audio_element_output(self, buf, len);
}

static audio_element_handle_t swvol_create(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open       = swvol_open;
    cfg.process    = swvol_process;
    cfg.close      = swvol_close;
    cfg.destroy    = swvol_destroy;
    cfg.tag        = "swvol";
    cfg.task_stack   = 2304;
    cfg.buffer_len   = 4096;
    cfg.task_core    = 1;
    cfg.stack_in_ext = true;   /* PSRAM — save internal DRAM */
    return audio_element_init(&cfg);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Decoder — esp_decoder auto-detect MP3/AAC/M4A/TS
 * ══════════════════════════════════════════════════════════════════════════ */

static audio_element_handle_t create_decoder(void) {
    esp_decoder_cfg_t cfg = DEFAULT_ESP_DECODER_CONFIG();
    cfg.task_core     = 1;
    cfg.stack_in_ext  = true;
    cfg.plus_enable   = true;
    audio_decoder_t decoder_list[] = {
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_M4A_DECODER_CONFIG(),
        DEFAULT_ESP_TS_DECODER_CONFIG(),
    };
    return esp_decoder_init(&cfg, decoder_list,
                            sizeof(decoder_list) / sizeof(decoder_list[0]));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pipeline build / teardown
 * ══════════════════════════════════════════════════════════════════════════ */

/* Convert user URI → esp_audio-compatible form */
static void to_esp_uri(const char *in, char *out, int out_sz) {
    if (strncmp(in, "http", 4) == 0)      snprintf(out, out_sz, "%s", in);
    else if (in[0] == '/')                snprintf(out, out_sz, "file://%s", in + 1);
    else                                  snprintf(out, out_sz, "%s", in);
}

static bool pipeline_build(void)
{
    /* ── Pipeline ── */
    audio_pipeline_cfg_t pl_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pl_cfg);
    if (!s_pipeline) { ESP_LOGE(TAG, "pipeline init failed"); return false; }

    /* ── Reader: fatfs (SD) or http ── */
    audio_element_handle_t reader = NULL;
    if (strncmp(s_esp_uri, "http", 4) == 0) {
        http_stream_cfg_t http = HTTP_STREAM_CFG_DEFAULT();
        http.type = AUDIO_STREAM_READER;
        http.crt_bundle_attach = esp_crt_bundle_attach;
        reader = http_stream_init(&http);
    } else {
        fatfs_stream_cfg_t fs = FATFS_STREAM_CFG_DEFAULT();
        fs.type = AUDIO_STREAM_READER;
        reader = fatfs_stream_init(&fs);
    }
    if (!reader) { ESP_LOGE(TAG, "reader init failed"); goto fail; }
    audio_element_set_uri(reader, s_esp_uri);
    audio_pipeline_register(s_pipeline, reader, "reader");

    /* ── Decoder (auto-detect format) ── */
    audio_element_handle_t decoder = create_decoder();
    if (!decoder) { ESP_LOGE(TAG, "decoder init failed"); goto fail; }
    audio_pipeline_register(s_pipeline, decoder, "decoder");

    /* ── Software volume filter ── */
    s_softvol_el = swvol_create();
    if (!s_softvol_el) { ESP_LOGE(TAG, "softvol init failed"); goto fail; }
    audio_pipeline_register(s_pipeline, s_softvol_el, "softvol");

    /* ── I2S output ── */
    i2s_stream_cfg_t i2s = I2S_STREAM_CFG_DEFAULT();
    i2s.type         = AUDIO_STREAM_WRITER;
    i2s.stack_in_ext  = true;   /* PSRAM — save internal DRAM for SDMMC DMA */
    i2s.std_cfg.gpio_cfg.bclk = I2S_BCLK;
    i2s.std_cfg.gpio_cfg.ws   = I2S_LRCLK;
    i2s.std_cfg.gpio_cfg.dout = I2S_DOUT;
    i2s.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    i2s.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    s_i2s_el = i2s_stream_init(&i2s);
    if (!s_i2s_el) { ESP_LOGE(TAG, "i2s init failed"); goto fail; }
    audio_pipeline_register(s_pipeline, s_i2s_el, "i2s");

    /* ── Link: reader → decoder → softvol → i2s ── */
    const char *tags[] = {"reader", "decoder", "softvol", "i2s"};
    if (audio_pipeline_link(s_pipeline, tags, 4) != ESP_OK) {
        ESP_LOGE(TAG, "pipeline link failed"); goto fail;
    }

    return true;

fail:
    if (s_pipeline) { audio_pipeline_deinit(s_pipeline); s_pipeline = NULL; }
    s_i2s_el    = NULL;
    s_softvol_el = NULL;
    return false;
}

static void pipeline_teardown(void)
{
    if (s_pipeline) {
        audio_pipeline_stop(s_pipeline);
        audio_pipeline_wait_for_stop(s_pipeline);
        audio_pipeline_deinit(s_pipeline);
        s_pipeline = NULL;
    }
    s_i2s_el    = NULL;
    s_softvol_el = NULL;
    s_playing   = false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

bool audio_player_init(void) {
    if (s_inited) return true;
    gpio_config_t out = { .pin_bit_mask = BIT64(PIN_AP_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(PIN_AP_EN, 1);
    s_inited = true;
    ESP_LOGI(TAG, "amp enabled (HT6872)");
    return true;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void capture_position(void) {
    /* Time position from I2S output (PCM bytes → seconds) */
    if (s_i2s_el) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(s_i2s_el, &info) == ESP_OK && info.sample_rates) {
            s_resume_sec = (int)(info.byte_pos / (info.sample_rates *
                                   info.channels * (info.bits / 8)));
            s_last_time_sec = s_resume_sec;
        }
    }
    /* Compressed byte offset from the decoder — for precise seek on rebuild */
    s_resume_byte = 0;
    audio_element_handle_t decoder =
        audio_pipeline_get_el_by_tag(s_pipeline, "decoder");
    if (decoder) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(decoder, &info) == ESP_OK) {
            s_resume_byte = info.byte_pos;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool audio_player_play(const char *url) {
    if (!url || !url[0]) return false;

    /* If same URL was released (download stole memory), resume from saved position */
    if (s_released && strcmp(url, s_uri) == 0) {
        ESP_LOGI(TAG, "play: resume after release (byte %lld)", s_resume_byte);
        audio_player_pause(false);   /* rebuild + seek to saved position */
        return s_playing;
    }

    pipeline_teardown();
    s_released      = false;
    s_resume_sec    = 0;
    s_resume_byte   = 0;
    s_last_time_sec = 0;
    to_esp_uri(url, s_esp_uri, sizeof(s_esp_uri));
    strncpy(s_uri, url, sizeof(s_uri) - 1);

    if (!pipeline_build()) return false;

    audio_pipeline_run(s_pipeline);
    s_playing = true;
    ESP_LOGI(TAG, "play: %s", s_esp_uri);
    return true;
}

void audio_player_stop(void) {
    capture_position();
    pipeline_teardown();
    s_released    = false;
    s_resume_sec  = 0;
    s_resume_byte = 0;
}

void audio_player_pause(bool pause) {
    if (pause) {
        if (!s_pipeline) return;
        audio_pipeline_pause(s_pipeline);
        s_playing  = false;
        s_released = false;
        ESP_LOGI(TAG, "paused");
    } else {
        /* Rebuild if pipeline was released (download stole the memory) */
        if (s_released || !s_pipeline) {
            if (!s_uri[0]) return;
            if (!pipeline_build()) return;

            /* Precise seek: skip compressed bytes already consumed by decoder */
            if (s_resume_byte > 0) {
                audio_element_handle_t reader =
                    audio_pipeline_get_el_by_tag(s_pipeline, "reader");
                if (reader) {
                    audio_element_info_t info = {0};
                    audio_element_getinfo(reader, &info);
                    info.byte_pos = s_resume_byte;
                    audio_element_setinfo(reader, &info);
                }
            }
            audio_pipeline_run(s_pipeline);
            s_released = false;
            ESP_LOGI(TAG, "resume after release (byte %lld, ~%ds)",
                     s_resume_byte, s_resume_sec);
        } else {
            audio_pipeline_resume(s_pipeline);
            ESP_LOGI(TAG, "resumed");
        }
        s_playing = true;
    }
}

void audio_player_release(void) {
    if (!s_pipeline) return;
    capture_position();
    pipeline_teardown();
    s_released = true;
    ESP_LOGI(TAG, "released (pos %ds saved)", s_resume_sec);
}

void audio_player_set_volume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 200) vol = 200;
    s_volume = vol;

    /* HW volume: ES8156 DAC (headphone) — capped at 0dB (100%) */
    int hw_vol = vol > 100 ? 100 : vol;
    int reg = 0x1F + hw_vol * (0xBF - 0x1F) / 100;
    if (s_es8156) es8156_write_reg(s_es8156, 0x14, (uint8_t)reg);

    /* SW volume: softvol reads s_volume directly, handles >100% with soft-clip */

    flash_set_i32("settings", "volume", vol);
    ESP_LOGI(TAG, "volume %d%% (hw %d%% reg 0x%02X, sw %d%%)",
             vol, hw_vol, reg, vol);
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
    audio_player_set_volume(s_volume);
    app_event_register(on_key_volume_event);
    ESP_LOGI(TAG, "codec init (volume %d%%, es8156=%p)", s_volume, (void *)s_es8156);
}

bool audio_player_is_playing(void) { return s_playing; }

int audio_player_get_position_sec(void) {
    if (s_pipeline && s_playing && s_i2s_el) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(s_i2s_el, &info) == ESP_OK && info.sample_rates) {
            s_last_time_sec = (int)(info.byte_pos / (info.sample_rates *
                                      info.channels * (info.bits / 8)));
        }
        /* Detect finish: if i2s element stopped, mark playing = false */
        audio_element_state_t st = audio_element_get_state(s_i2s_el);
        if (st == AEL_STATE_FINISHED || st == AEL_STATE_STOPPED ||
            st == AEL_STATE_ERROR) {
            s_playing = false;
        }
    }
    return s_last_time_sec;
}

bool audio_player_is_active(void) { return s_pipeline != NULL; }
