/*
 * NomadCast — TDP Power Consumption Test (t_TDP)
 *
 * Three test modes, selected by the TDP_MODE macro:
 *
 *   TDP_MODE_DOWNLOAD — WiFi download cycling (radio + SD writes)
 *   TDP_MODE_SCREEN   — display always-on (SPI + backlight)
 *   TDP_MODE_AUDIO    — loop playback of /sdcard/test.m4a via speaker
 *
 * Every INTERVAL_MIN minutes, a timestamp + battery voltage is appended
 * to /sdcard/tdp_log.csv.  The test runs until the battery is exhausted.
 * Read the CSV file from the SD card afterwards to determine runtime.
 *
 * === Leisound V1 Pins ===
 *   EN_POWER    GPIO46    board peripheral power
 *   SD CLK/CMD/D0  GPIO1/14/2   SDMMC 1-bit
 *   LCD SPI     GPIO10/11/12/13/45  CS/MOSI/SCK/MISO/DC
 *   LCD RST     GPIO8     shared with touch reset
 *   BAT ADC     ADC1_CH2 (GPIO3)  x2 voltage divider
 *   I2S BCK/WS/DOUT  GPIO5/6/7
 *   AMP_EN      GPIO21    HT6872 amp enable (verified in audio_player.c)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ── Math fallback ──────────────────────────────────────────────────────── */

#ifndef M_PI
#define M_PI  3.14159265358979323846
#endif

/* ── Mode selection ─────────────────────────────────────────────────────── */

#define TDP_MODE_DOWNLOAD  1
#define TDP_MODE_SCREEN    2
#define TDP_MODE_AUDIO     3

/* === USER CONFIGURATION ================================================== */

#define TDP_MODE        TDP_MODE_DOWNLOAD   /* <-- change mode here */

#define WIFI_SSID       "CMCC-ayrw"         /* DOWNLOAD mode only */
#define WIFI_PASSWORD   "y2bap7kw"

#define TEST_URL        "http://speedtest.tele2.net/1MB.zip"  /* DOWNLOAD: ~1 MB test file */
#define INTERVAL_MIN    30                  /* log every N minutes */
#define LOG_PATH        "/sdcard/tdp_log.csv"

/* ══════════════════════════════════════════════════════════════════════════ */

static const char *TAG = "t_tdp";

/* ── Board pins ─────────────────────────────────────────────────────────── */

#define PIN_EN_POWER    GPIO_NUM_46
#define PIN_SD_CLK      GPIO_NUM_1
#define PIN_SD_CMD      GPIO_NUM_14
#define PIN_SD_DAT0     GPIO_NUM_2
#define PIN_SD_MOUNT    "/sdcard"

/* Battery ADC (same as sys/battery_monitor/battery.c) */
#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_2       /* GPIO3 */
#define BAT_DIVIDER     2
#define BAT_SAMPLES     16

/* ── Download mode ──────────────────────────────────────────────────────── */
#if TDP_MODE == TDP_MODE_DOWNLOAD

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define DL_BUF_SIZE     8192
#define DL_TIMEOUT_MS   120000    /* 2-minute timeout for a 1 MB file */

/* ── Screen mode ────────────────────────────────────────────────────────── */
#elif TDP_MODE == TDP_MODE_SCREEN

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"

#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_45
#define PIN_LCD_RST     GPIO_NUM_8
#define PIN_SPI_SCK     GPIO_NUM_12
#define PIN_SPI_MOSI    GPIO_NUM_11
#define PIN_SPI_MISO    GPIO_NUM_13
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (40 * 1000 * 1000)
#define LCD_W           240
#define LCD_H           320

/* ── Audio mode ─────────────────────────────────────────────────────────── */
#elif TDP_MODE == TDP_MODE_AUDIO

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "aac_decoder.h"
#include "board_pins_config.h"

#define PIN_AMP_EN      GPIO_NUM_21    /* HT6872 amp */
#define AUDIO_FILE      "/sdcard/test.m4a"

/* ADF custom board: provide I2S pins to i2s_stream element */
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *cfg)
{
    (void)port;
    cfg->bck_io_num   = GPIO_NUM_5;
    cfg->ws_io_num    = GPIO_NUM_6;
    cfg->data_out_num = GPIO_NUM_7;
    cfg->data_in_num  = I2S_GPIO_UNUSED;
    cfg->mck_io_num   = I2S_GPIO_UNUSED;
    return ESP_OK;
}

static audio_pipeline_handle_t  s_pipeline;
static audio_element_handle_t   s_reader, s_decoder, s_i2s;
static audio_event_iface_handle_t s_evt;

#else
#error "Invalid TDP_MODE — must be TDP_MODE_DOWNLOAD, TDP_MODE_SCREEN, or TDP_MODE_AUDIO"
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Common: Battery voltage reading
 * ══════════════════════════════════════════════════════════════════════════ */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;

/* Averaged real battery voltage in millivolts (after x2 divider). */
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

/* Li-ion OCV (mV) → percent.  Same table as sys/battery_monitor. */
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

    ESP_LOGI(TAG, "ADC init OK (ADC1_CH%d / GPIO3)", BAT_ADC_CHANNEL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Common: SD card
 * ══════════════════════════════════════════════════════════════════════════ */

static bool sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = PIN_SD_CLK;
    slot.cmd   = PIN_SD_CMD;
    slot.d0    = PIN_SD_DAT0;
    slot.width = 1;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(PIN_SD_MOUNT, &host, &slot, &mount, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", PIN_SD_MOUNT);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Common: CSV logging
 * ══════════════════════════════════════════════════════════════════════════ */

static FILE *s_log_file = NULL;
static int64_t s_start_us = 0;

static const char *mode_name(void)
{
    switch (TDP_MODE) {
    case TDP_MODE_DOWNLOAD: return "DOWNLOAD";
    case TDP_MODE_SCREEN:   return "SCREEN";
    case TDP_MODE_AUDIO:    return "AUDIO";
    default:                return "UNKNOWN";
    }
}

static bool log_open(void)
{
    s_log_file = fopen(LOG_PATH, "a");
    if (!s_log_file) {
        ESP_LOGE(TAG, "Cannot open %s", LOG_PATH);
        return false;
    }

    /* Write CSV header if the file is empty */
    fseek(s_log_file, 0, SEEK_END);
    long size = ftell(s_log_file);
    if (size == 0) {
        fprintf(s_log_file, "# NomadCast TDP Power Test\n");
        fprintf(s_log_file, "# Mode: %s\n", mode_name());
        fprintf(s_log_file, "# Interval: %d min\n", INTERVAL_MIN);
        fprintf(s_log_file, "index,elapsed_min,voltage_mv,battery_pct\n");
        fflush(s_log_file);
        ESP_LOGI(TAG, "Created log header in %s", LOG_PATH);
    } else {
        /* Re-opening after a crash/resume — keep appending */
        ESP_LOGI(TAG, "Appending to existing log (%ld bytes)", size);
    }

    s_start_us = esp_timer_get_time();
    return true;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Mode: DOWNLOAD — WiFi + HTTP download cycling
 * ══════════════════════════════════════════════════════════════════════════ */
#if TDP_MODE == TDP_MODE_DOWNLOAD

static volatile bool s_got_ip = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi: got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_got_ip = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi: disconnected, reconnecting...");
        s_got_ip = false;
        esp_wifi_connect();
    }
}

static bool wifi_connect(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid,     sizeof(sta.sta.ssid),     "%s", WIFI_SSID);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", WIFI_PASSWORD);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi connecting to '%s'...", WIFI_SSID);

    /* Wait up to 20 seconds for 802.11 association */
    for (int i = 0; i < 40; i++) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi associated — RSSI=%d, waiting for IP...", ap.rssi);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Wait up to 15 seconds for DHCP to assign an IP */
    for (int i = 0; i < 30; i++) {
        if (s_got_ip) return true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "WiFi IP timeout");
    return false;
}

static bool do_download(void)
{
    esp_http_client_config_t cfg = {
        .url = TEST_URL,
        .timeout_ms = DL_TIMEOUT_MS,
        .buffer_size = DL_BUF_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "dl: client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dl: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "dl: HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    const char *tmp_path = "/sdcard/tdp_test.bin";
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "dl: cannot open %s", tmp_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char *buf = malloc(DL_BUF_SIZE);
    int total = 0;
    if (buf) {
        while (1) {
            int n = esp_http_client_read(client, buf, DL_BUF_SIZE);
            if (n <= 0) break;
            fwrite(buf, 1, n, f);
            total += n;
        }
        free(buf);
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total > 0) {
        ESP_LOGI(TAG, "dl: downloaded %d bytes OK", total);
        unlink(tmp_path);  /* delete — we only needed the power draw */
        return true;
    } else {
        ESP_LOGE(TAG, "dl: 0 bytes received");
        unlink(tmp_path);
        return false;
    }
}

/* ── DOWNLOAD main ──────────────────────────────────────────────────────── */

static bool mode_init(void)
{
    if (!wifi_connect()) return false;
    return true;
}

static void mode_pre_log(int index, int elapsed)
{
    ESP_LOGI(TAG, "--- cycle %d (elapsed %d min): downloading ---", index, elapsed);
    bool ok = do_download();
    ESP_LOGI(TAG, "--- cycle %d: download %s ---", index, ok ? "OK" : "FAIL");
}

static void mode_poll(void) { /* nothing to poll */ }

#endif /* TDP_MODE_DOWNLOAD */

/* ══════════════════════════════════════════════════════════════════════════
 * Mode: SCREEN — display always-on
 * ══════════════════════════════════════════════════════════════════════════ */
#if TDP_MODE == TDP_MODE_SCREEN

static esp_lcd_panel_handle_t s_panel;

static bool screen_init(void)
{
    /* Power on then hardware reset the LCD */
    gpio_config_t rst = { .pin_bit_mask = BIT64(PIN_LCD_RST), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* SPI2 bus */
    spi_bus_config_t spi = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi, SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                              &io_cfg, &io));

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Fill white — maximum backlight power draw */
    int buf_sz = LCD_W * LCD_H;
    uint16_t *fb = malloc(buf_sz * sizeof(uint16_t));
    if (fb) {
        for (int i = 0; i < buf_sz; i++) fb[i] = 0xFFFF;  /* RGB565 white */
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, fb);
        free(fb);
    }

    ESP_LOGI(TAG, "Screen init OK (white, always-on)");
    return true;
}

static bool mode_init(void) { return screen_init(); }
static void mode_pre_log(int index, int elapsed) { (void)index; (void)elapsed; }
static void mode_poll(void) { /* nothing to poll */ }

#endif /* TDP_MODE_SCREEN */

/* ══════════════════════════════════════════════════════════════════════════
 * Mode: AUDIO — loop M4A file playback via ADF pipeline
 * ══════════════════════════════════════════════════════════════════════════ */
#if TDP_MODE == TDP_MODE_AUDIO

static bool audio_init(void)
{
    /* Amp enable */
    gpio_config_t amp = { .pin_bit_mask = BIT64(PIN_AMP_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&amp);
    gpio_set_level(PIN_AMP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Check file exists */
    struct stat st;
    if (stat(AUDIO_FILE, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", AUDIO_FILE);
        ESP_LOGE(TAG, "Put test.m4a on SD card root!");
        return false;
    }
    ESP_LOGI(TAG, "Found: %s (%ld bytes)", AUDIO_FILE, (long)st.st_size);

    /* Build pipeline: fatfs → aac_decoder → i2s */
    audio_pipeline_cfg_t pl_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pl_cfg);
    if (!s_pipeline) { ESP_LOGE(TAG, "pipeline init fail"); return false; }

    fatfs_stream_cfg_t fs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fs_cfg.type = AUDIO_STREAM_READER;
    s_reader = fatfs_stream_init(&fs_cfg);
    audio_element_set_uri(s_reader, AUDIO_FILE);

    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    s_decoder = aac_decoder_init(&aac_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    s_i2s = i2s_stream_init(&i2s_cfg);

    audio_pipeline_register(s_pipeline, s_reader,  "file");
    audio_pipeline_register(s_pipeline, s_decoder, "dec");
    audio_pipeline_register(s_pipeline, s_i2s,     "i2s");
    const char *tags[] = {"file", "dec", "i2s"};
    audio_pipeline_link(s_pipeline, tags, 3);

    /* Event listener for playback finish + music info */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipeline, s_evt);

    /* Start first playback */
    audio_pipeline_run(s_pipeline);
    ESP_LOGI(TAG, "Audio init OK — playing %s in loop", AUDIO_FILE);
    return true;
}

static void audio_loop_check(void)
{
    /* Check for playback events (non-blocking) */
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(s_evt, &msg, 0);
    if (ret != ESP_OK) return;

    /* Decoder parsed audio header → configure I2S clock */
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.source == (void *)s_decoder
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t info = {0};
        audio_element_getinfo(s_decoder, &info);
        ESP_LOGI(TAG, "Music: %d Hz, %d bit, %d ch",
                 info.sample_rates, info.bits, info.channels);
        audio_element_setinfo(s_i2s, &info);
        i2s_stream_set_clk(s_i2s, info.sample_rates, info.bits, info.channels);
        return;
    }

    /* Playback finished → loop.
     * ADF reports AEL_STATUS_STATE_FINISHED when the reader hits EOF and the
     * pipeline drains naturally.  AEL_STATUS_STATE_STOPPED is only for explicit
     * stop (we issue it ourselves on the loop restart).  Check from ANY element
     * (reader, decoder, or I2S) — different ADF versions report on different ones. */
    if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
        int status = (int)msg.data;
        if (status == AEL_STATUS_STATE_FINISHED) {
            ESP_LOGI(TAG, "Playback ended → restarting...");
            audio_pipeline_stop(s_pipeline);
            audio_pipeline_wait_for_stop(s_pipeline);
            audio_pipeline_reset_ringbuffer(s_pipeline);
            audio_pipeline_reset_elements(s_pipeline);
            audio_pipeline_reset_items_state(s_pipeline);  /* seek back to 0 */
            audio_pipeline_run(s_pipeline);

            /* Drain stale events (our own stop sends STOPPED, etc.) */
            audio_event_iface_msg_t drain;
            while (audio_event_iface_listen(s_evt, &drain, 0) == ESP_OK) {}
        }
    }
}

static bool mode_init(void) { return audio_init(); }
static void mode_pre_log(int index, int elapsed) { (void)index; (void)elapsed; }
static void mode_poll(void) { audio_loop_check(); }

#endif /* TDP_MODE_AUDIO */

/* ══════════════════════════════════════════════════════════════════════════
 * Main — common loop for all modes
 * ══════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  NomadCast TDP Power Test — MODE=%s", mode_name());
    ESP_LOGI(TAG, "  Interval: %d min  |  Log: %s", INTERVAL_MIN, LOG_PATH);
    ESP_LOGI(TAG, "============================================");

    /* [1] Board power */
    gpio_config_t pwr = { .pin_bit_mask = BIT64(PIN_EN_POWER), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Board power ON (GPIO46)");

    /* [2] SD card */
    if (!sd_mount()) {
        ESP_LOGE(TAG, "FATAL: SD card required for logging");
        goto fail;
    }

    /* [3] Battery ADC */
    adc_init();

    /* [4] Open log file BEFORE mode init — ADF's fatfs_stream may remount
     *     the FATFS instance, so we create the log header now to ensure it
     *     exists.  Actual data writes use open/append/fsync/close per cycle. */
    if (!log_open()) {
        ESP_LOGE(TAG, "FATAL: cannot open log file");
        goto fail;
    }
    if (s_log_file) { fclose(s_log_file); s_log_file = NULL; }

    /* [5] Mode-specific init */
    if (!mode_init()) {
        ESP_LOGE(TAG, "FATAL: mode init failed");
        goto fail;
    }

    /* [6] Main loop — log + wait INTERVAL_MIN minutes.
     *     Each cycle opens the CSV for append, writes one line, fsyncs, and
     *     closes — survives SD remounts by the ADF pipeline. */
    ESP_LOGI(TAG, "=== Starting test loop ===");
    s_start_us = esp_timer_get_time();
    for (int cycle = 1; ; cycle++) {
        int64_t t0 = esp_timer_get_time();
        int elapsed = (int)((t0 - s_start_us) / 60000000LL);

        /* Per-mode action before logging (e.g. download) */
        mode_pre_log(cycle, elapsed);

        /* Write this cycle's reading (open → append → fsync → close) */
        {
            int mv  = battery_voltage_mv();
            int pct = battery_percent_from_mv(mv);

            s_log_file = fopen(LOG_PATH, "a");
            if (s_log_file) {
                fprintf(s_log_file, "%d,%d,%d,%d\n", cycle, elapsed, mv, pct);
                fflush(s_log_file);
                fsync(fileno(s_log_file));
                fclose(s_log_file);
                s_log_file = NULL;
                ESP_LOGI(TAG, "[%d] elapsed=%d min  voltage=%d mV  battery=%d%%",
                         cycle, elapsed, mv, pct);
            } else {
                ESP_LOGW(TAG, "Cannot open %s for append", LOG_PATH);
            }
        }

        /* Wait INTERVAL_MIN minutes, polling mode-specific events */
        ESP_LOGI(TAG, "Waiting %d minutes for next cycle...", INTERVAL_MIN);
        for (int m = 0; m < INTERVAL_MIN; m++) {
            for (int s = 0; s < 60; s++) {
                mode_poll();   /* e.g. check audio loop events */
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

fail:
    ESP_LOGE(TAG, "=== TDP test stopped ===");
    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
}
