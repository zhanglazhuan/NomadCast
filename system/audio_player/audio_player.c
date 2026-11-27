/*
 * NomadCast Audio Player — manual audio_pipeline (no esp_audio .a)
 *
 * Pipeline:  reader (fatfs or http) → esp_decoder → softvol → i2s_stream
 *
 * Volume:    ES8156 I2C register (headphone) + software PCM scale
 *
 * Lifecycle: play()  = build → run
 *            stop()  = stop → deinit
 *            pause() = teardown  (saves position)
 *            resume()= rebuild   (seeks to saved position)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "esp_decoder.h"
#include "audio_player.h"
#include "play_history.h"
#include "driver/i2c_master.h"
#include "es8156.h"
#include "app_event.h"
#include "flash_store.h"
#include "nomadcast.h"
#include "server_config.h"

static const char *TAG = "audio_player";

/* The device ID is read from NVS once, on the UI thread (internal-DRAM stack),
 * and reused here for HTTP headers.  It must NOT be re-read from an ADF
 * element task: those run with PSRAM stacks (see the audio_thread.c vendored
 * patch → MALLOC_CAP_SPIRAM), and an NVS/flash read on such a task trips
 * esp_task_stack_is_sane_cache_disabled() in spi_flash/cache_utils.c
 * (assert → reboot).  audio_player_init() fills this before any pipeline task
 * is spawned. */
static char s_device_id[24] = {0};

void audio_player_log_memory(const char *where)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "mem[%s]: internal_free=%u largest=%u", where,
             (unsigned)free_bytes, (unsigned)largest);
}

#define log_memory audio_player_log_memory

/* ── I2S pins (from nomadcast.h) ───────────────────────────────────── */
#define I2S_BCLK  NOMADCAST_PIN_I2S_BCLK
#define I2S_LRCLK NOMADCAST_PIN_I2S_WS
#define I2S_DOUT  NOMADCAST_PIN_I2S_DOUT
#define I2S_MCLK  NOMADCAST_PIN_I2S_MCLK   /* GPIO13 → ES8156 MCLK */
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
     * from the ESP32's I2S MCLK on GPIO13 — without it the DAC clock is
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
    audio_event_iface_handle_t events;
    es8156_handle_t codec;
    bool initialized;
    bool playing;
    bool released;
    int resume_sec;
    int64_t resume_byte;
    int last_time_sec;
    int base_sec;               /* absolute position at which the current playback
                                 * started (resume offset); 0 for a fresh play */
    int duration_sec;           /* true stream duration from the decoder (0 until known) */
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
    if (!msg || !msg->el) return 0;

    /* Before connecting, attach the shared API key to streams that target our
     * own aggregation server (the /api/radio/stream proxy).  Direct CDN URLs
     * must never leak the key to third parties, so gate strictly on the base
     * URL prefix — same convention as the podcast HTTP client. */
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        if (strncmp(s_player.esp_uri, NOMADCAST_SERVER_BASE,
                    strlen(NOMADCAST_SERVER_BASE)) == 0) {
            esp_http_client_handle_t hc = (esp_http_client_handle_t)msg->http_client;
            esp_http_client_set_header(hc, "X-Api-Key", NOMADCAST_API_KEY);
            if (s_device_id[0] != '\0')
                esp_http_client_set_header(hc, "X-Device-Id", s_device_id);
        }
        return 0;
    }

    /* Live HLS: the http_stream playlist parser drives these two events when
     * enable_playlist_parser is set.  Advance to the next segment on track end,
     * and re-fetch the (live) playlist when the current snapshot is exhausted —
     * exactly the ADF pipeline_living_stream pattern. */
    if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        return http_stream_fetch_again(msg->el);
    }

    if (msg->event_id != HTTP_STREAM_FINISH_REQUEST) return 0;
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
        /* Parse m3u8/HLS playlists.  The parser only engages when the upstream
         * answers with a playlist content-type (or a .m3u8 URL), so plain MP3/
         * AAC/M4A streams are unaffected.  Live HLS keeps playing across
         * segments via the FINISH_TRACK/FINISH_PLAYLIST event handler above. */
        http.enable_playlist_parser = true;
        /* A zero range size means "from offset to EOF".  This is required for
         * pause/resume: http_stream will emit Range: bytes=<byte_pos>-. */
        http.request_range_size = 0;
        /* Live streams have no known length and are jitter-prone.  The default
         * 20KB ring buffer only absorbs ~1.5s at 128kbps; bump to 64KB so a
         * brief network stall doesn't underrun the decoder and stutter audio.
         * This only raises the max buffering depth — playback still starts as
         * soon as the first frame arrives. */
        http.out_rb_size = 64 * 1024;
        reader = http_stream_init(&http);
    } else {
        fatfs_stream_cfg_t fs = FATFS_STREAM_CFG_DEFAULT();
        fs.type = AUDIO_STREAM_READER;
        /* The fatfs reader defaults to an internal-DRAM task stack (4KB). With
         * the UI/model competing for internal DRAM the largest free block can
         * drop below 4KB, so xTaskCreate fails and audio_pipeline_resume fails
         * (the "stuck" symptom). Put the reader stack in PSRAM like the decoder/
         * softvol/i2s elements — ADF's own http_stream does the same. */
        fs.ext_stack = true;
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

    /* The decoder only knows the stream format after parsing its header.
     * Keep a listener attached so the main loop can apply that format to I2S. */
    audio_event_iface_cfg_t event_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_player.events = audio_event_iface_init(&event_cfg);
    if (!s_player.events) {
        ESP_LOGE(TAG, "audio event interface init failed"); goto fail;
    }
    if (audio_pipeline_set_listener(s_player.pipeline, s_player.events) != ESP_OK) {
        ESP_LOGE(TAG, "pipeline listener setup failed"); goto fail;
    }

    return true;

fail:
    if (s_player.events) {
        if (s_player.pipeline) audio_pipeline_remove_listener(s_player.pipeline);
        audio_event_iface_destroy(s_player.events);
        s_player.events = NULL;
    }
    if (s_player.pipeline) { audio_pipeline_deinit(s_player.pipeline); s_player.pipeline = NULL; }
    s_player.i2s    = NULL;
    s_player.softvol = NULL;
    return false;
}

static void pipeline_teardown(void)
{
    if (s_player.pipeline) {
        audio_pipeline_stop(s_player.pipeline);
        /* Bounded wait: a stuck decoder task may never report STOPPED, and the
         * default audio_pipeline_wait_for_stop() blocks with portMAX_DELAY,
         * freezing the UI. Time out after 2s and let audio_pipeline_deinit()
         * force-delete the zombie task below. */
        audio_pipeline_wait_for_stop_with_ticks(s_player.pipeline, pdMS_TO_TICKS(2000));
        if (s_player.events) audio_pipeline_remove_listener(s_player.pipeline);
    }
    if (s_player.events) {
        audio_event_iface_destroy(s_player.events);
        s_player.events = NULL;
    }
    if (s_player.pipeline) {
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
    /* Cache the device ID here, on the caller's internal-DRAM stack, so the
     * http_stream element task never has to touch NVS/flash itself. */
    if (s_device_id[0] == '\0')
        flash_get_device_id(s_device_id, sizeof(s_device_id));

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
    /* Time position from I2S output (PCM bytes → seconds).  I2S counts from the
     * start of THIS pipeline, so add base_sec — the resume offset — to get the
     * absolute track position (a resumed file otherwise reports 0 at its 9s
     * point, making the progress bar count up from 0 instead of resuming). */
    if (s_player.i2s) {
        audio_element_info_t info = {0};
        if (audio_element_getinfo(s_player.i2s, &info) == ESP_OK && info.sample_rates) {
            s_player.resume_sec = s_player.base_sec +
                (int)(info.byte_pos / (info.sample_rates *
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

/* Case-insensitive check that `path` ends in the given extension (dot included). */
static bool path_has_ext(const char *path, const char *ext) {
    if (!path || !ext) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    size_t plen = strlen(dot), elen = strlen(ext);
    if (plen != elen) return false;
    for (size_t i = 0; i < elen; i++) {
        char a = dot[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

/* Classify a local audio file by sniffing its leading bytes.  The on-SD
 * extension is unreliable: the downloader names every file `.aac`, but the
 * bytes can be a remuxed AAC-ADTS stream, a raw-copied MP3, or an MP4/M4A
 * container whose remux fell back to `mp4_copy_file`.  The decoder sniffs
 * content too, so resume must route by content — not extension — or a MP4
 * mislabelled `.aac` gets ADTS-align-seeked and stalls. */
typedef enum {
    LOCAL_CODEC_UNKNOWN = 0,
    LOCAL_CODEC_ADTS,
    LOCAL_CODEC_MP4,
    LOCAL_CODEC_MP3,
} local_codec_t;

static local_codec_t sniff_local_codec(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return LOCAL_CODEC_UNKNOWN;
    unsigned char b[16];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);

    /* MP4/M4A container: an ISO box header — type at offset 4. */
    if (n >= 8) {
        if (memcmp(b + 4, "ftyp", 4) == 0 || memcmp(b + 4, "moov", 4) == 0 ||
            memcmp(b + 4, "mdat", 4) == 0 || memcmp(b + 4, "free", 4) == 0 ||
            memcmp(b + 4, "wide", 4) == 0 || memcmp(b + 4, "skip", 4) == 0 ||
            memcmp(b + 4, "styp", 4) == 0 || memcmp(b + 4, "moof", 4) == 0 ||
            memcmp(b + 4, "sidx", 4) == 0)
            return LOCAL_CODEC_MP4;
    }

    /* MP3 with an ID3v2 tag prepended. */
    if (n >= 3 && memcmp(b, "ID3", 3) == 0) return LOCAL_CODEC_MP3;

    /* ADTS AAC: 0xFFF sync, layer bits 00 (b[1] in {0xF0,0xF1,0xF8,0xF9}). */
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) return LOCAL_CODEC_ADTS;

    /* MP3 without ID3: 0xFFE sync (b[1] top 3 bits set, layer bits non-zero). */
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return LOCAL_CODEC_MP3;

    return LOCAL_CODEC_UNKNOWN;
}

/* Align `*byte_pos` forward to the start of the next ADTS frame in `path`.
 * The reader's saved byte_pos is a block boundary — i.e. mid-frame — and the
 * precompiled AAC decoder does NOT resync to an arbitrary offset: fed mid-frame
 * data it stalls (reads ahead while producing 0 PCM, the "卡住" state).  ADTS
 * frames are self-describing (12-bit 0xFFF sync + 13-bit frame_length), so scan
 * forward for a sync word whose following frame also starts with a sync — that
 * confirms a true frame boundary.  The next frame is within one frame length
 * (<= 8191 bytes) of the block boundary, so the scan is bounded to a small
 * window: never walk the whole file (a 10+ MB scan stalls the UI thread for
 * tens of seconds and false-positives on non-ADTS content). */
static bool align_adts_frame(const char *path, int64_t *byte_pos) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    const int64_t k_window = 64 * 1024;
    int64_t chunk_off = *byte_pos;
    int64_t limit = *byte_pos + k_window;
    unsigned char buf[1024];

    while (chunk_off < limit) {
        if (fseek(f, chunk_off, SEEK_SET) != 0) break;
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n < 8) break;

        for (size_t i = 0; i + 7 <= n; i++) {
            /* 12-bit sync + layer==00 (matches aac_file_probe's mask). */
            if (buf[i] != 0xFF || (buf[i + 1] & 0xF6) != 0xF0) continue;

            /* 13-bit frame length (ADTS header bits 30-43). */
            int flen = ((buf[i + 3] & 0x03) << 11) | (buf[i + 4] << 3) | (buf[i + 5] >> 5);
            if (flen < 7 || flen > 8191) continue;   /* implausible frame header */

            /* Confirm: the NEXT frame must also begin with a sync word. */
            int64_t next = chunk_off + (int64_t)i + flen;
            if (fseek(f, next, SEEK_SET) == 0) {
                unsigned char h2[2];
                if (fread(h2, 1, 2, f) == 2 &&
                    h2[0] == 0xFF && (h2[1] & 0xF6) == 0xF0) {
                    *byte_pos = chunk_off + (int64_t)i;
                    fclose(f);
                    return true;
                }
            }
        }

        if (n < sizeof(buf)) break;               /* EOF */
        chunk_off += (int64_t)(n - 7);            /* overlap so a straddling sync is caught */
    }

    fclose(f);
    return false;
}

/* Seek a freshly-built local-file pipeline forward to s_player.resume_byte.
 * The auto-detect decoder must first read the container header to pick the
 * codec, so the reader opens at position 0, the decoder consumes the header,
 * and only then do we drop the buffered header bytes and seek the reader. */
static void apply_local_byte_seek(void) {
    if (s_player.resume_byte <= 0) return;
    if (strncmp(s_player.esp_uri, "http", 4) == 0) return;

    /* Route by sniffed CONTENT, not the on-SD extension (the downloader names
     * every file `.aac`, but a remux-failure fallback leaves MP4 or MP3 bytes
     * under that name). */
    local_codec_t codec = sniff_local_codec(s_player.uri);

    /* MP4 containers (M4A/MP4/M4B): ADF's precompiled AAC decoder is a
     * positional stream parser — frame boundaries come from the moov sample
     * table and the data is consumed sequentially, so it cannot resync to an
     * arbitrary compressed byte offset.  Seeking the reader mid-atom/mid-frame
     * makes it error and stall: the reader reads ahead while no PCM is ever
     * produced.  The codec lib exposes no seek API, so byte-offset resume is
     * unsupported for these files — play from the start instead. */
    if (codec == LOCAL_CODEC_MP4 ||
        path_has_ext(s_player.uri, ".m4a") ||
        path_has_ext(s_player.uri, ".mp4") ||
        path_has_ext(s_player.uri, ".m4b")) {
        ESP_LOGI(TAG, "resume: %s is MP4 (byte-seek unsupported), play from 0",
                 s_player.uri);
        s_player.resume_byte = 0;
        s_player.resume_sec  = 0;
        return;
    }

    /* ADTS (.aac): the saved byte offset is a block boundary, not a frame
     * boundary.  The precompiled AAC decoder does not resync to an arbitrary
     * offset and stalls (reads ahead, 0 PCM) when fed mid-frame data — align
     * forward to the next clean frame start so the seek lands on a decodable
     * frame.  If no boundary is found (corrupt / non-ADTS content), play from
     * the start rather than stall. */
    if (codec == LOCAL_CODEC_ADTS) {
        int64_t aligned = s_player.resume_byte;
        if (align_adts_frame(s_player.uri, &aligned)) {
            if (aligned != s_player.resume_byte)
                ESP_LOGI(TAG, "resume: ADTS frame aligned %lld -> %lld",
                         (long long)s_player.resume_byte, (long long)aligned);
            s_player.resume_byte = aligned;
        } else {
            ESP_LOGW(TAG, "resume: no ADTS boundary near %lld, play from 0",
                     (long long)s_player.resume_byte);
            s_player.resume_byte = 0;
            s_player.resume_sec  = 0;
        }
    }

    /* MP3 (sync-word resync) and other tolerant formats keep the raw byte-offset
     * seek below.  UNKNOWN content also keeps the offset: the decoder sniffs it
     * too, and resetting to 0 for an unknown-but-seekable format would lose the
     * resume position for no reason. */

    audio_element_handle_t reader  = audio_pipeline_get_el_by_tag(s_player.pipeline, "reader");
    audio_element_handle_t decoder = audio_pipeline_get_el_by_tag(s_player.pipeline, "decoder");
    if (!reader || !decoder) return;

    /* Wait until the decoder has finished opening (state RUNNING), so the
     * codec is set and detection has consumed the header. */
    for (int i = 0; i < 300; i++) {
        audio_element_state_t st = audio_element_get_state(decoder);
        if (st == AEL_STATE_RUNNING) break;
        if (st == AEL_STATE_ERROR || st == AEL_STATE_STOPPED ||
            st == AEL_STATE_FINISHED) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* audio_element_setinfo only writes the byte_pos FIELD — the real file
     * seek happens inside _fatfs_open on (re)open. So pause (close) the reader,
     * set the offset, drop the header bytes already fed into the decoder's
     * input, then resume (reopen) it. */
    audio_element_pause(reader);
    audio_element_reset_input_ringbuf(decoder);
    audio_element_info_t info = {0};
    audio_element_getinfo(reader, &info);
    info.byte_pos = s_player.resume_byte;
    audio_element_setinfo(reader, &info);
    audio_element_resume(reader, 0.0f, portMAX_DELAY);
}

/* Persist the current local-file position to the playback history, so the next
 * play() of the same path resumes where the user left off.  Only called from
 * stop()/pause() on the LVGL/app task (never an ADF element task). */
static void persist_position_if_local(void) {
    if (!audio_player_is_local_source()) return;
    if (s_player.resume_byte <= 0) return;

    /* Finished (within 3s of the end): drop the entry so the next play starts
     * from 0 rather than seeking to a byte offset at EOF. */
    if (s_player.duration_sec > 0 &&
        s_player.resume_sec >= s_player.duration_sec - 3) {
        play_history_remove(s_player.uri);
        play_history_save();
        return;
    }

    play_history_upsert(s_player.uri, s_player.resume_byte, s_player.resume_sec);
    play_history_save();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Drag-to-seek — time → byte resolution + rebuild-seek
 * ══════════════════════════════════════════════════════════════════════════ */

/* Side-car seek-index format, written by apps/podcast/mp4_remux.c next to a
 * remuxed .aac.  Binary, little-endian:
 *   u32 magic   = SEEK_INDEX_MAGIC
 *   u32 version = 1
 *   u32 frame_ms
 *   u32 entry_count
 *   entry_count × { u32 sec; u32 byte; }   (byte = frame-aligned ADTS offset) */
#define SEEK_INDEX_MAGIC   0x4443494Eu   /* "NCID" — MUST match mp4_remux.c */
#define SEEK_INDEX_VERSION 1u
typedef struct { uint32_t sec; uint32_t byte; } seek_idx_ent_t;

/* Load the side-car index for `path` and resolve target_sec to a frame-aligned
 * byte offset (greatest entry with sec <= target_sec).  False when absent. */
static bool seek_index_resolve(const char *path, int target_sec, int64_t *out_byte) {
    char idx_path[2608];
    snprintf(idx_path, sizeof(idx_path), "%s.idx", path);
    FILE *f = fopen(idx_path, "rb");
    if (!f) return false;

    uint32_t magic = 0, ver = 0, fms = 0, cnt = 0;
    bool ok = (fread(&magic, 4, 1, f) == 1 && magic == SEEK_INDEX_MAGIC &&
               fread(&ver, 4, 1, f) == 1 && ver == SEEK_INDEX_VERSION &&
               fread(&fms, 4, 1, f) == 1 &&
               fread(&cnt, 4, 1, f) == 1);
    (void)fms;
    if (!ok || cnt == 0 || cnt > (4u * 3600u)) { fclose(f); return false; }

    seek_idx_ent_t *idx = (seek_idx_ent_t *)malloc((size_t)cnt * sizeof(seek_idx_ent_t));
    if (!idx) { fclose(f); return false; }
    if (fread(idx, sizeof(seek_idx_ent_t), cnt, f) != cnt) { free(idx); fclose(f); return false; }
    fclose(f);

    int64_t byte = -1;
    uint32_t lo = 0, hi = cnt;          /* greatest sec <= target_sec */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (idx[mid].sec <= (uint32_t)target_sec) { byte = idx[mid].byte; lo = mid + 1; }
        else hi = mid;
    }
    free(idx);
    if (byte < 0) return false;
    *out_byte = byte;
    return true;
}

/* Fallback when no index: assume CBR and scale by file size / duration.
 * target_sec == 0 resolves to byte 0 (play from the start). */
static bool seek_linear_estimate(const char *path, int target_sec, int duration_sec,
                                 int64_t *out_byte) {
    if (duration_sec <= 0 || target_sec < 0) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 0) return false;
    *out_byte = (int64_t)sz * target_sec / duration_sec;
    return true;
}

bool audio_player_seek_to_sec(int target_sec, int duration_hint) {
    if (!s_player.pipeline || target_sec < 0) return false;
    if (!s_player.uri[0]) return false;

    const bool local = audio_player_is_local_source();

    /* Prefer the decoder's measured duration, but fall back to the caller's hint
     * (the player page already knows the real length from the local-library
     * cache / feed).  ADTS streams carry no duration in-band, so the decoder
     * may report 0 — without the hint a linear (no-index) seek would fail. */
    int dur = s_player.duration_sec > 0 ? s_player.duration_sec : duration_hint;

    /* Clamp to just short of the end so a drag to the last second doesn't land
     * at EOF (the reader seek would then fail and reset to 0). */
    if (dur > 1 && target_sec >= dur)
        target_sec = dur - 1;

    if (local) {
        /* Local file: resolve the target second to a compressed byte offset. */
        local_codec_t codec = sniff_local_codec(s_player.uri);
        if (codec == LOCAL_CODEC_MP4 ||
            path_has_ext(s_player.uri, ".m4a") ||
            path_has_ext(s_player.uri, ".mp4") ||
            path_has_ext(s_player.uri, ".m4b")) {
            ESP_LOGW(TAG, "seek: %s is MP4 — byte seek unsupported", s_player.uri);
            return false;
        }

        int64_t byte = -1;
        if (codec == LOCAL_CODEC_ADTS && seek_index_resolve(s_player.uri, target_sec, &byte)) {
            ESP_LOGI(TAG, "seek: index %ds → byte %lld", target_sec, (long long)byte);
        } else if (seek_linear_estimate(s_player.uri, target_sec, dur, &byte)) {
            ESP_LOGI(TAG, "seek: linear %ds → byte %lld", target_sec, (long long)byte);
        } else {
            ESP_LOGW(TAG, "seek: cannot resolve %ds (no index, dur=%d)", target_sec, dur);
            return false;
        }
        s_player.resume_byte = byte;
        s_player.resume_sec  = target_sec;
    } else {
        /* Network stream: seek by server-side time — rebuild the URI with a
         * `start=N` query param (the proxy/transcoder starts from that second). */
        s_player.resume_byte = 0;
        s_player.resume_sec  = target_sec;
        ESP_LOGI(TAG, "seek: stream %ds (start param)", target_sec);
    }

    /* The codec has no in-place flush/seek, so this is a full teardown + fresh
     * decode-state build — the ~100-300ms seek cost (same as resume-rebuild). */
    if (s_player.pipeline) pipeline_teardown();

    /* For a network seek, re-apply the `start=` param before rebuilding. */
    if (!local) {
        char resume_uri[sizeof(s_player.esp_uri)];
        build_resume_uri(resume_uri, sizeof(resume_uri));
        to_esp_uri(resume_uri, s_player.esp_uri, sizeof(s_player.esp_uri));
    }

    if (!pipeline_build()) return false;
    audio_pipeline_run(s_player.pipeline);
    if (local) apply_local_byte_seek();
    s_player.base_sec      = s_player.resume_sec;
    s_player.last_time_sec = s_player.resume_sec;
    s_player.released      = false;
    s_player.playing       = true;

    if (local) {
        play_history_upsert(s_player.uri, s_player.resume_byte, s_player.resume_sec);
        play_history_save();
    }

    log_memory("seek");
    ESP_LOGI(TAG, "seeked to %ds (byte %lld)", target_sec, (long long)s_player.resume_byte);
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void audio_player_process(void)
{
    if (!s_player.events || !s_player.pipeline) return;

    audio_event_iface_msg_t message;
    while (audio_event_iface_listen(s_player.events, &message, 0) == ESP_OK) {
        audio_element_handle_t decoder =
            audio_pipeline_get_el_by_tag(s_player.pipeline, "decoder");
        if (message.source_type != AUDIO_ELEMENT_TYPE_ELEMENT ||
            message.source != (void *)decoder ||
            message.cmd != AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            continue;
        }

        audio_element_info_t info = {0};
        esp_err_t err = audio_element_getinfo(decoder, &info);
        if (err == ESP_OK) audio_element_setinfo(s_player.i2s, &info);
        if (err == ESP_OK) {
            err = i2s_stream_set_clk(s_player.i2s, info.sample_rates,
                                     info.bits, info.channels);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2S synced: %d Hz, %d bit, %d ch",
                     info.sample_rates, info.bits, info.channels);
        } else {
            ESP_LOGE(TAG, "I2S sync failed: %s (%d Hz, %d bit, %d ch)",
                     esp_err_to_name(err), info.sample_rates,
                     info.bits, info.channels);
        }

        /* The decoder just parsed the container header — capture the true
         * duration (moov/mvhd for M4A, frame scan for MP3) so the player's
         * progress bar can stop trusting the possibly-wrong feed duration.
         * ADF reports info.duration in MILLISECONDS. */
        int d = info.duration > 0 ? info.duration / 1000 : 0;
        if (d <= 0 && info.total_bytes > 0 && info.bps > 0)
            d = (int)(info.total_bytes * 8 / info.bps);
        if (d <= 0 && info.total_bytes > 0 && info.sample_rates > 0 &&
            info.channels > 0 && info.bits > 0) {
            int byte_rate = info.sample_rates * info.channels * (info.bits / 8);
            if (byte_rate > 0) d = (int)(info.total_bytes / byte_rate);
        }
        ESP_LOGI(TAG, "info: dur_ms=%d total=%lld pos=%lld bps=%d rate=%d ch=%d bits=%d -> %ds",
                 info.duration, (long long)info.total_bytes, (long long)info.byte_pos,
                 info.bps, info.sample_rates, info.channels, info.bits, d);
        if (d > 0) s_player.duration_sec = d;
    }
}

bool audio_player_play(const char *url) {
    if (!url || !url[0]) return false;

    /* If same URL was released (download stole memory), resume from saved position */
    if (s_player.released && strcmp(url, s_player.uri) == 0) {
        ESP_LOGI(TAG, "play: resume after release (byte %lld)", s_player.resume_byte);
        audio_player_pause(false);   /* rebuild + seek to saved position */
        return s_player.playing;
    }

    /* Record the currently-playing file before switching away to the new one.
     * Without this, playing B right after A drops A's resume position from the
     * history — the switch path never went through stop()/pause(true). */
    if (s_player.pipeline) {
        capture_position();
        persist_position_if_local();
    }

    pipeline_teardown();
    s_player.released      = false;
    s_player.resume_sec    = 0;
    s_player.resume_byte   = 0;
    s_player.last_time_sec = 0;
    s_player.base_sec      = 0;
    s_player.duration_sec  = 0;
    to_esp_uri(url, s_player.esp_uri, sizeof(s_player.esp_uri));
    strncpy(s_player.uri, url, sizeof(s_player.uri) - 1);

    /* Resume a previously-played local file from its saved byte offset. */
    if (audio_player_is_local_source()) {
        play_history_entry_t e;
        if (play_history_find(url, &e) && e.resume_byte > 0) {
            s_player.resume_byte = e.resume_byte;
            s_player.resume_sec  = e.resume_sec;
            ESP_LOGI(TAG, "play: resume from history (byte %lld, ~%ds)",
                     (long long)e.resume_byte, e.resume_sec);
        }
    }

    if (!pipeline_build()) return false;

    audio_pipeline_run(s_player.pipeline);
    apply_local_byte_seek();
    /* The I2S position counts from 0 regardless of where the reader was seeked,
     * so record the resume offset as the base and add it to every position read
     * (get_position_sec/capture_position) to report the absolute track time. */
    s_player.base_sec      = s_player.resume_sec;
    s_player.last_time_sec = s_player.resume_sec;
    s_player.playing = true;

    /* Record the new file immediately so it appears in "最近播放" even before
     * any stop/pause, and so a resumed file moves back to the top of the list.
     * For a fresh play resume_byte is 0 (list shows 0:00); stop/pause/switch
     * later overwrite it with the real position. */
    if (audio_player_is_local_source()) {
        play_history_upsert(url, s_player.resume_byte, s_player.resume_sec);
        play_history_save();
    }

    log_memory("play");
    ESP_LOGI(TAG, "play: %s", s_player.esp_uri);
    return true;
}

void audio_player_stop(void) {
    capture_position();
    persist_position_if_local();
    pipeline_teardown();
    s_player.released    = false;
    s_player.resume_sec  = 0;
    s_player.resume_byte = 0;
    s_player.base_sec    = 0;
    s_player.duration_sec = 0;
}

void audio_player_pause(bool pause) {
    if (pause) {
        if (!s_player.pipeline) return;
        capture_position();          /* save decoder byte offset + i2s time */
        persist_position_if_local();
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

        /* Transcoded HTTP streams resume by server-side time; local files seek
         * by byte offset after decoder initialization. */
        apply_local_byte_seek();
        s_player.base_sec      = s_player.resume_sec;
        s_player.last_time_sec = s_player.resume_sec;
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
            s_player.last_time_sec = s_player.base_sec +
                (int)(info.byte_pos / (info.sample_rates *
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

int audio_player_get_duration_sec(void) { return s_player.duration_sec; }

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
