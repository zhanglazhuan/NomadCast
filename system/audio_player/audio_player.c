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
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

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
#include "nomadcast_v1.h"

static const char *TAG = "audio_player";

void audio_player_log_memory(const char *where)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "mem[%s]: internal_free=%u largest=%u", where,
             (unsigned)free_bytes, (unsigned)largest);
}

#define log_memory audio_player_log_memory

/* ── I2S pins (from nomadcast_v1.h) ───────────────────────────────────── */
#define I2S_BCLK  NOMADCAST_PIN_I2S_BCLK
#define I2S_LRCLK NOMADCAST_PIN_I2S_WS
#define I2S_DOUT  NOMADCAST_PIN_I2S_DOUT
#define I2S_MCLK  NOMADCAST_PIN_I2S_MCLK   /* GPIO1 → ES8156 MCLK */
#define PIN_AP_EN NOMADCAST_PIN_AMP_EN  /* HT6872 amp enable */
#define PIN_HP_DET NOMADCAST_PIN_HP_DETECT  /* headphone detect (HIGH=inserted) */

/* i2s_stream calls get_i2s_pins() (CONFIG_AUDIO_BOARD_CUSTOM). */
#include "board_pins_config.h"
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *cfg) {
    (void)port;
    cfg->bck_io_num   = I2S_BCLK;
    cfg->ws_io_num    = I2S_LRCLK;
    cfg->data_out_num = I2S_DOUT;
    cfg->data_in_num  = I2S_GPIO_UNUSED;
    /* ES8156 is a slave DAC that needs a master clock (Ratio=256). Drive it
     * from the ESP32's I2S MCLK on GPIO1 — without it the DAC clock is
     * unstable and produces crackle. */
    cfg->mck_io_num   = I2S_MCLK;
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * State
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s;
    audio_element_handle_t softvol;
    es8156_handle_t codec;
    bool initialized;
    bool playing;
    bool released;
    int resume_sec;
    int64_t resume_byte;
    int last_time_sec;
    char uri[2600];
    char esp_uri[2600];
    volatile int volume;
    int speaker_volume;
    int headphone_volume;
    bool output_headphone;
    QueueHandle_t headphone_queue;
    int headphone_last_level;
} audio_player_state_t;

static audio_player_state_t s_player = {
    .volume = 50,
    .speaker_volume = 50,
    .headphone_volume = 50,
    .headphone_last_level = -1,
};

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

    int vol = s_player.volume;   /* shared, 0-200 */
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

/* A server that ignores Range would otherwise return HTTP 200 from byte 0,
 * while ADF keeps the requested byte_pos, producing duplicated/wrong audio.
 * Reject that reconnect so the caller can surface a stream failure instead of
 * silently pretending that resume succeeded. */
static int http_stream_event_handler(http_stream_event_msg_t *msg)
{
    if (!msg || msg->event_id != HTTP_STREAM_FINISH_REQUEST || !msg->el) return 0;
    audio_element_info_t info = {0};
    audio_element_getinfo(msg->el, &info);
    int status = esp_http_client_get_status_code((esp_http_client_handle_t)msg->http_client);
    if (info.byte_pos > 0 && status != 206) {
        ESP_LOGE(TAG, "HTTP server ignored resume Range (status=%d, offset=%lld)",
                 status, (long long)info.byte_pos);
        return -1;
    }
    return 0;
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
    s_player.pipeline = audio_pipeline_init(&pl_cfg);
    if (!s_player.pipeline) { ESP_LOGE(TAG, "pipeline init failed"); return false; }

    /* ── Reader: fatfs (SD) or http ── */
    audio_element_handle_t reader = NULL;
    if (strncmp(s_player.esp_uri, "http", 4) == 0) {
        http_stream_cfg_t http = HTTP_STREAM_CFG_DEFAULT();
        http.type = AUDIO_STREAM_READER;
        http.crt_bundle_attach = esp_crt_bundle_attach;
        http.event_handle = http_stream_event_handler;
        /* A zero range size means "from offset to EOF".  This is required for
         * pause/resume: http_stream will emit Range: bytes=<byte_pos>-. */
        http.request_range_size = 0;
        reader = http_stream_init(&http);
    } else {
        fatfs_stream_cfg_t fs = FATFS_STREAM_CFG_DEFAULT();
        fs.type = AUDIO_STREAM_READER;
        reader = fatfs_stream_init(&fs);
    }
    if (!reader) { ESP_LOGE(TAG, "reader init failed"); goto fail; }
    audio_element_set_uri(reader, s_player.esp_uri);
    audio_pipeline_register(s_player.pipeline, reader, "reader");

    /* ── Decoder (auto-detect format) ── */
    audio_element_handle_t decoder = create_decoder();
    if (!decoder) { ESP_LOGE(TAG, "decoder init failed"); goto fail; }
    audio_pipeline_register(s_player.pipeline, decoder, "decoder");

    /* ── Software volume filter ── */
    s_player.softvol = swvol_create();
    if (!s_player.softvol) { ESP_LOGE(TAG, "softvol init failed"); goto fail; }
    audio_pipeline_register(s_player.pipeline, s_player.softvol, "softvol");

    /* ── I2S output ── */
    i2s_stream_cfg_t i2s = I2S_STREAM_CFG_DEFAULT();
    i2s.type         = AUDIO_STREAM_WRITER;
    i2s.stack_in_ext  = true;   /* PSRAM — save internal DRAM for SDMMC DMA */
    i2s.std_cfg.gpio_cfg.bclk = I2S_BCLK;
    i2s.std_cfg.gpio_cfg.ws   = I2S_LRCLK;
    i2s.std_cfg.gpio_cfg.dout = I2S_DOUT;
    i2s.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    i2s.std_cfg.gpio_cfg.mclk = I2S_MCLK;   /* feed ES8156 a stable master clock */
    s_player.i2s = i2s_stream_init(&i2s);
    if (!s_player.i2s) { ESP_LOGE(TAG, "i2s init failed"); goto fail; }
    audio_pipeline_register(s_player.pipeline, s_player.i2s, "i2s");

    /* ── Link: reader → decoder → softvol → i2s ── */
    const char *tags[] = {"reader", "decoder", "softvol", "i2s"};
    if (audio_pipeline_link(s_player.pipeline, tags, 4) != ESP_OK) {
        ESP_LOGE(TAG, "pipeline link failed"); goto fail;
    }

    return true;

fail:
    if (s_player.pipeline) { audio_pipeline_deinit(s_player.pipeline); s_player.pipeline = NULL; }
    s_player.i2s    = NULL;
    s_player.softvol = NULL;
    return false;
}

static void pipeline_teardown(void)
{
    if (s_player.pipeline) {
        audio_pipeline_stop(s_player.pipeline);
        audio_pipeline_wait_for_stop(s_player.pipeline);
        audio_pipeline_deinit(s_player.pipeline);
        s_player.pipeline = NULL;
    }
    s_player.i2s    = NULL;
    s_player.softvol = NULL;
    s_player.playing   = false;
}

static void build_resume_uri(char *out, size_t out_sz)
{
    if (s_player.resume_sec <= 0 || strncmp(s_player.uri, "http", 4) != 0) {
        snprintf(out, out_sz, "%s", s_player.uri);
        return;
    }
    snprintf(out, out_sz, "%s%sstart=%d", s_player.uri,
             strchr(s_player.uri, '?') ? "&" : "?", s_player.resume_sec);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

bool audio_player_init(void) {
    if (s_player.initialized) return true;
    gpio_config_t out = { .pin_bit_mask = BIT64(PIN_AP_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    /* Amp LEVEL is owned by audio_player_headphone_detect_init() — do NOT
     * force HIGH here, or a boot-inserted headphone would be overridden back
     * to speaker on the first play(). */
    s_player.initialized = true;
    ESP_LOGI(TAG, "amp GPIO ready (HT6872)");
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Headphone detect — AMP_EN GPIO18 edge → toggles HT6872 amp (AP_EN GPIO44)
 * Debounced detection ported from debug/t_key.
 * ══════════════════════════════════════════════════════════════════════════ */

#define HP_DEBOUNCE_MS 50


/* Require the level to be stable for HP_DEBOUNCE_MS before returning. */
static int hp_debounced_level(void)
{
    int level = gpio_get_level(PIN_HP_DET);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HP_DEBOUNCE_MS));
        int l = gpio_get_level(PIN_HP_DET);
        if (l == level) return level;
        level = l;   /* still bouncing — restart the timer */
    }
}

/* Inserted (level 1) → mute speaker; removed (level 0) → speaker on. */
static void hp_apply_routing(int level)
{
    s_player.output_headphone = level != 0;
    gpio_set_level(PIN_AP_EN, level ? 0 : 1);
    s_player.volume = s_player.output_headphone ? s_player.headphone_volume : s_player.speaker_volume;
    /* Apply the channel-specific remembered level without rewriting NVS. */
    int hw_limit = s_player.output_headphone ? 100 : 40;
    int hw_vol = s_player.volume > hw_limit ? hw_limit : s_player.volume;
    if (s_player.codec) {
        int reg = 0x1F + hw_vol * (0xBF - 0x1F) / 100;
        es8156_write_reg(s_player.codec, 0x14, (uint8_t)reg);
    }
    ESP_LOGI(TAG, "headphone %s → speaker %s",
             level ? "INSERTED" : "REMOVED", level ? "OFF" : "ON");
}

static void IRAM_ATTR hp_isr_handler(void *arg)
{
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(s_player.headphone_queue, &pin, NULL);
}

static void hp_detect_task(void *arg)
{
    (void)arg;

    /* Wait for the detect circuit to settle after power-up, take the stable
     * level as the baseline, then drain any edges queued during startup. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    int level = hp_debounced_level();
    s_player.headphone_last_level = level;
    hp_apply_routing(level);

    uint32_t pin;
    while (xQueueReceive(s_player.headphone_queue, &pin, portMAX_DELAY) == pdTRUE) {
        if (pin != (uint32_t)PIN_HP_DET) continue;
        level = hp_debounced_level();
        if (level != s_player.headphone_last_level) {
            s_player.headphone_last_level = level;
            hp_apply_routing(level);
        }
    }
}

void audio_player_headphone_detect_init(void)
{
    /* Amp enable as output — its level is owned by hp_detect_task. */
    gpio_config_t amp = { .pin_bit_mask = BIT64(PIN_AP_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&amp);

    /* Headphone detect: input, both edges, floating (board has its own pull). */
    gpio_config_t hp = {
        .intr_type    = GPIO_INTR_ANYEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT64(PIN_HP_DET),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&hp);

    s_player.headphone_queue = xQueueCreate(4, sizeof(uint32_t));
    if (!s_player.headphone_queue) {
        ESP_LOGE(TAG, "headphone detect: queue alloc failed");
        return;
    }

    /* Shared ISR service — GT911 already installs it; tolerate that. */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "headphone detect: ISR service failed: %s", esp_err_to_name(e));
    }
    gpio_isr_handler_add(PIN_HP_DET, hp_isr_handler, (void *)(uintptr_t)PIN_HP_DET);

    xTaskCreate(hp_detect_task, "hp_detect", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "headphone detect ready (GPIO%d → amp GPIO%d)",
             PIN_HP_DET, PIN_AP_EN);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void capture_position(void) {
    /* Time position from I2S output (PCM bytes → seconds) */
    if (s_player.i2s) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(s_player.i2s, &info) == ESP_OK && info.sample_rates) {
            s_player.resume_sec = (int)(info.byte_pos / (info.sample_rates *
                                   info.channels * (info.bits / 8)));
            s_player.last_time_sec = s_player.resume_sec;
        }
    }
    /* Compressed byte offset from the READER (file position) — reliably tracked
     * by fatfs_stream via audio_element_update_byte_pos. The decoder's byte_pos
     * is NOT advanced by audio_element_input, so it can't be used for seeking. */
    s_player.resume_byte = 0;
    audio_element_handle_t reader =
        audio_pipeline_get_el_by_tag(s_player.pipeline, "reader");
    if (reader) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(reader, &info) == ESP_OK) {
            s_player.resume_byte = info.byte_pos;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool audio_player_play(const char *url) {
    if (!url || !url[0]) return false;

    /* If same URL was released (download stole memory), resume from saved position */
    if (s_player.released && strcmp(url, s_player.uri) == 0) {
        ESP_LOGI(TAG, "play: resume after release (byte %lld)", s_player.resume_byte);
        audio_player_pause(false);   /* rebuild + seek to saved position */
        return s_player.playing;
    }

    pipeline_teardown();
    s_player.released      = false;
    s_player.resume_sec    = 0;
    s_player.resume_byte   = 0;
    s_player.last_time_sec = 0;
    to_esp_uri(url, s_player.esp_uri, sizeof(s_player.esp_uri));
    strncpy(s_player.uri, url, sizeof(s_player.uri) - 1);

    if (!pipeline_build()) return false;

    audio_pipeline_run(s_player.pipeline);
    s_player.playing = true;
    log_memory("play");
    ESP_LOGI(TAG, "play: %s", s_player.esp_uri);
    return true;
}

void audio_player_stop(void) {
    capture_position();
    pipeline_teardown();
    s_player.released    = false;
    s_player.resume_sec  = 0;
    s_player.resume_byte = 0;
}

void audio_player_pause(bool pause) {
    if (pause) {
        if (!s_player.pipeline) return;
        capture_position();          /* save decoder byte offset + i2s time */
        /* Pause only the sink.  ADF's pipeline pause propagates a reset-like
         * command into the M4A/AAC decoder; resuming then re-runs container
         * detection at a compressed offset and can fail with decoder error 560.
         * Leaving reader/decoder blocked on their ring buffers preserves the
         * exact decode state and keeps resume in place. */
        esp_err_t err = s_player.i2s ? audio_element_pause(s_player.i2s) : ESP_FAIL;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pause failed: %s", esp_err_to_name(err));
            return;
        }
        /* Keep the reader/decoder and their container state alive.  Rebuilding
         * an M4A pipeline and seeking to a compressed byte offset can land in
         * the middle of an atom/frame and produces decoder error 560/noise. */
        s_player.released = false;
        s_player.playing  = false;
        log_memory("pause");
        ESP_LOGI(TAG, "paused in place (byte %lld, ~%ds)", s_player.resume_byte, s_player.resume_sec);
    } else {
        if (s_player.pipeline && !s_player.playing && !s_player.released) {
            esp_err_t err = s_player.i2s ? audio_element_resume(s_player.i2s, 0.0f,
                                                             portMAX_DELAY) : ESP_FAIL;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "resume failed: %s", esp_err_to_name(err));
                return;
            }
            s_player.playing = true;
            log_memory("resume-in-place");
            ESP_LOGI(TAG, "resumed in place (byte %lld, ~%ds)", s_player.resume_byte, s_player.resume_sec);
            return;
        }
        /* Rebuild every time. The auto-detect decoder must read the container
         * header to pick the codec — if we seek straight to s_player.resume_byte it
         * reads mid-file data, detects garbage ("PCM"), and aborts the whole
         * pipeline. So the fresh reader opens at file position 0, the decoder
         * detects the codec from the header, and only then do we drop the
         * buffered header bytes and seek the reader forward to the saved
         * compressed-byte offset. */
        if (!s_player.uri[0]) return;
        if (s_player.pipeline) pipeline_teardown();
        char resume_uri[sizeof(s_player.esp_uri)];
        build_resume_uri(resume_uri, sizeof(resume_uri));
        to_esp_uri(resume_uri, s_player.esp_uri, sizeof(s_player.esp_uri));
        if (!pipeline_build()) return;

        audio_pipeline_run(s_player.pipeline);

        /* Transcoded HTTP streams resume by server-side time; local files can
         * still seek by byte offset after decoder initialization. */
        if (s_player.resume_byte > 0 && strncmp(s_player.esp_uri, "http", 4) != 0) {
            audio_element_handle_t reader  = audio_pipeline_get_el_by_tag(s_player.pipeline, "reader");
            audio_element_handle_t decoder = audio_pipeline_get_el_by_tag(s_player.pipeline, "decoder");
            if (reader && decoder) {
                /* Wait until the decoder has finished opening (state RUNNING),
                 * so the codec is set and detection has consumed the header. */
                for (int i = 0; i < 300; i++) {
                    audio_element_state_t st = audio_element_get_state(decoder);
                    if (st == AEL_STATE_RUNNING) break;
                    if (st == AEL_STATE_ERROR || st == AEL_STATE_STOPPED ||
                        st == AEL_STATE_FINISHED) break;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                /* audio_element_setinfo only writes the byte_pos FIELD — the real
                 * file seek happens inside _fatfs_open on (re)open. So pause
                 * (close) the reader, set the offset, drop the header bytes it
                 * already fed into the decoder's input, then resume (reopen) it. */
                audio_element_pause(reader);
                audio_element_reset_input_ringbuf(decoder);
                audio_element_info_t info = {0};
                audio_element_getinfo(reader, &info);
                info.byte_pos = s_player.resume_byte;
                audio_element_setinfo(reader, &info);
                audio_element_resume(reader, 0.0f, portMAX_DELAY);
            }
        }
        s_player.released = false;
        s_player.playing  = true;
        log_memory("resume-rebuild");
        ESP_LOGI(TAG, "resumed (byte %lld, ~%ds)", s_player.resume_byte, s_player.resume_sec);
    }
}

void audio_player_release(void) {
    if (!s_player.pipeline) return;
    capture_position();
    pipeline_teardown();
    s_player.released = true;
    log_memory("release");
    ESP_LOGI(TAG, "released (pos %ds saved)", s_player.resume_sec);
}

void audio_player_set_volume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 200) vol = 200;
    s_player.volume = vol;
    if (s_player.output_headphone) {
        s_player.headphone_volume = vol;
        flash_set_i32("settings", "volume_hp", vol);
    } else {
        s_player.speaker_volume = vol;
        flash_set_i32("settings", "volume_speaker", vol);
    }

    /* HT6872 speaker gain is substantially higher than the headphone path.
     * Keep the DAC input attenuated on speaker output to prevent the amp from
     * clipping; headphones retain the full user range. */
    bool headphone = s_player.output_headphone;
    int hw_limit = headphone ? 100 : 40;
    int hw_vol = vol > hw_limit ? hw_limit : vol;
    int reg = 0x1F + hw_vol * (0xBF - 0x1F) / 100;
    if (s_player.codec) es8156_write_reg(s_player.codec, 0x14, (uint8_t)reg);

    /* SW volume: softvol reads s_player.volume directly, handles >100% with soft-clip */

    flash_set_i32("settings", "volume", vol);
    ESP_LOGI(TAG, "volume %d%% (%s hw %d%% reg 0x%02X, sw %d%%)",
             vol, headphone ? "headphone" : "speaker", hw_vol, reg, vol);
}

int audio_player_get_volume(void) { return s_player.volume; }

static void on_key_volume_event(app_event_t event, const void *data) {
    (void)data;
    if (event == APP_EVENT_KEY_VOL_UP)        audio_player_set_volume(s_player.volume + 10);
    else if (event == APP_EVENT_KEY_VOL_DOWN) audio_player_set_volume(s_player.volume - 10);
}

void audio_player_codec_init(i2c_master_bus_handle_t i2c_bus) {
    es8156_config_t cfg = {
        .i2c_bus = i2c_bus,
        .i2c_address = NOMADCAST_ES8156_ADDR,
    };
    if (es8156_initialize(&cfg, &s_player.codec) != ESP_OK ||
        es8156_configure(s_player.codec) != ESP_OK) {
        ESP_LOGW(TAG, "ES8156 init failed — volume control disabled");
        if (s_player.codec) {
            es8156_deinitialize(s_player.codec);
        }
        s_player.codec = NULL;
    }
    /* Auto-route speaker/headphone based on jack detect. */
    audio_player_headphone_detect_init();
    int legacy_volume = flash_get_i32("settings", "volume", 50);
    s_player.speaker_volume = flash_get_i32("settings", "volume_speaker", legacy_volume);
    s_player.headphone_volume = flash_get_i32("settings", "volume_hp", legacy_volume);
    s_player.volume = s_player.speaker_volume;
    audio_player_set_volume(s_player.volume);
    app_event_register(on_key_volume_event);
    ESP_LOGI(TAG, "codec init (volume %d%%, es8156=%p)", s_player.volume, (void *)s_player.codec);
}

bool audio_player_is_playing(void) { return s_player.playing; }

int audio_player_get_position_sec(void) {
    if (s_player.pipeline && s_player.playing && s_player.i2s) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(s_player.i2s, &info) == ESP_OK && info.sample_rates) {
            s_player.last_time_sec = (int)(info.byte_pos / (info.sample_rates *
                                      info.channels * (info.bits / 8)));
        }
        /* Detect finish: if i2s element stopped, mark playing = false */
        audio_element_state_t st = audio_element_get_state(s_player.i2s);
        if (st == AEL_STATE_FINISHED || st == AEL_STATE_STOPPED ||
            st == AEL_STATE_ERROR) {
            s_player.playing = false;
        }
    }
    return s_player.last_time_sec;
}

bool audio_player_is_active(void) { return s_player.pipeline != NULL; }

bool audio_player_memory_pressure(void)
{
    /* Keep a paused decoder alive whenever possible.  Release it only when
     * the largest internal block is unlikely to satisfy SD/HTTP allocations. */
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return free_bytes < (64 * 1024) || largest < (24 * 1024);
}

bool audio_player_is_local_source(void)
{
    const char *uri = s_player.esp_uri[0] ? s_player.esp_uri : s_player.uri;
    return uri[0] != '\0' && strncmp(uri, "http", 4) != 0;
}
