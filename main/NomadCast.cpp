/*
 * NomadCast — Main Entry Point
 *
 * ESP32-S3 Leisound V1 board. Proven init sequence from t_ui:
 *   1. Power ON → HW reset → Display (ST7789) → Touch (GT911) → LVGL
 *   2. App manager + Settings app → Launcher home UI → Gesture hooks
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "lvgl.h"

extern "C" {
#include "app_manager.h"
#include "launcher.h"
#include "app.h"
#include "view.h"       /* SETTINGS_PAGE_* + PAGE_NAVIGATE_TO */
#include "lv_page.h"    /* Page, lv_page_create */
#include "page_navigator.h"
#include "input.h"      /* input_init, input_event_t */
#include "app_event.h"  /* app_event_fire, APP_EVENT_KEY_PLAY_PAUSE */
#include "sleep_monitor.h"
#include "flash_store.h"
#include "clock.h"
#include "battery.h"
#include "audio_player.h"
#include "ota.h"
#include "gt911.h"
#include "nomadcast_v1.h"
#include "hal.h"
#include "wifi_cred.h"
#include "log_system.h"

/* SD card */
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
void settings_app_register(void);
	void podcast_app_register(void);
	void controller_process_rss(void);
	void controller_process_download(void);
}

#include "esp_check.h"  /* ESP_RETURN_ON_ERROR */

static const char *TAG = "NomadCast";

/* CJK font with Montserrat fallback for LV_SYMBOL_* glyphs */
extern "C" const struct _lv_font_t font_harmony;
static lv_font_t s_font_harmony_with_fb;  /* mutable copy — allows fallback chaining */
/* g_cjk_font defined with C linkage (used by lv_page.c etc.) */
extern "C" { const lv_font_t *g_cjk_font = &s_font_harmony_with_fb; }

/* ---- Pin map (NomadCast V1 — from nomadcast_v1.h) ---- */
#define PIN_EN_POWER    NOMADCAST_PIN_AP_POWER
#define PIN_SPI_SCK     NOMADCAST_PIN_SPI_SCK
#define PIN_SPI_MOSI    NOMADCAST_PIN_SPI_MOSI
#define PIN_SPI_MISO    NOMADCAST_PIN_SPI_MISO
#define PIN_LCD_CS      NOMADCAST_PIN_LCD_CS
#define PIN_LCD_DC      NOMADCAST_PIN_LCD_DC
#define PIN_LCD_RST     NOMADCAST_PIN_LCD_RST
#define PIN_LCD_POWER   NOMADCAST_PIN_LCD_POWER
#define PIN_BACKLIGHT   NOMADCAST_PIN_LCD_BL
#define PIN_I2C_SDA     NOMADCAST_PIN_I2C_SDA
#define PIN_I2C_SCL     NOMADCAST_PIN_I2C_SCL
#define PIN_TP_INT      NOMADCAST_PIN_TP_INT
#define PIN_TP_RST      NOMADCAST_PIN_TP_RST
#define LCD_W           240
#define LCD_H           320
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (40 * 1000 * 1000)

/* ---- Display (ST7789 via SPI2, esp_lcd) — proven from t_ui ---- */
static esp_lcd_panel_handle_t display_init(void)
{
    spi_bus_config_t spi_cfg = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1,
        .data6_io_num = -1, .data7_io_num = -1,
        .data_io_default_level = 0,
        .max_transfer_sz = LCD_W * 20 * 2,  /* match flush buffer height */
        .flags = 0, .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO, .intr_flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = SPI_FREQ_HZ, .trans_queue_depth = 10,
        .on_color_trans_done = NULL, .user_ctx = NULL,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .cs_ena_pretrans = 0, .cs_ena_posttrans = 0, .flags = {},
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .flags = {}, .vendor_config = NULL,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    /* Display stays OFF until launcher renders — prevents boot garbage */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
    return panel;
}

/* ---- HW reset (shared GPIO40 for LCD + GT911) ---- */
static void hw_reset(void)
{
    gpio_config_t cfg = { .pin_bit_mask = BIT64(PIN_TP_RST) | BIT64(PIN_TP_INT), .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&cfg);
    gpio_set_level(PIN_TP_INT, 0); gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_INT, 1);
    gpio_config_t in_cfg = { .pin_bit_mask = BIT64(PIN_TP_INT), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&in_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* ---- Touch (GT911 via drivers/gt911, interrupt-driven) ---- */
static gt911_dev_t    *s_gt911_dev  = NULL;
static gt911_config_t  s_gt911_cfg;  /* preserved for sleep→wake re-init */
static lv_indev_data_t s_touch_data;

/* Touch release debounce — GT911 may register brief release/press jitter
 * when finger hovers close to the screen.  We require a stable "released"
 * state for 40 ms before LVGL sees it.  Presses are reported immediately. */
#define TOUCH_RELEASE_DEBOUNCE_MS  40

static uint32_t          s_release_since_ms = 0;   /* lv_tick when finger lifted */
static lv_indev_state_t  s_stable_state     = LV_INDEV_STATE_RELEASED;
static lv_point_t        s_last_press_pt    = {0, 0};

static void on_gt911_touch(const gt911_touch_point_t *points, uint8_t count, void *ud)
{
    (void)ud;
    if (count > 0) {
        s_touch_data.state   = LV_INDEV_STATE_PRESSED;
        s_touch_data.point.x = points[0].x;
        s_touch_data.point.y = points[0].y;
    } else {
        s_touch_data.state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* ISR callback (on_gt911_touch) updates s_touch_data from gt911_irq_task.
     * We only apply release debounce — press is reported immediately. */
    uint32_t now = lv_tick_get();

    if (s_touch_data.state == LV_INDEV_STATE_PRESSED) {
        /* Instant press — clear any pending release debounce */
        s_release_since_ms = 0;
        s_stable_state     = LV_INDEV_STATE_PRESSED;
        s_last_press_pt    = s_touch_data.point;
    } else {
        /* Finger lifted — start debounce timer if not already running */
        if (s_stable_state == LV_INDEV_STATE_PRESSED) {
            if (s_release_since_ms == 0) {
                s_release_since_ms = now;
            }
            /* Only commit the release after the debounce window */
            if (now - s_release_since_ms >= TOUCH_RELEASE_DEBOUNCE_MS) {
                s_stable_state     = LV_INDEV_STATE_RELEASED;
                s_release_since_ms = 0;
            }
            /* During the debounce window, keep reporting PRESSED at last position */
        }
    }

    data->state = s_stable_state;
    data->point = s_last_press_pt;
}

/* ---- LVGL display flush callback ---- */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;

    /* RGB565 byte swap — LVGL outputs big-endian, ST7789 SPI expects little-endian */
    lv_draw_sw_rgb565_swap(px, w * h);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x1 + w, area->y1 + h, px);
    lv_display_flush_ready(disp);
}
static void lvgl_tick_cb(void *arg) { lv_tick_inc(1); }

/* ---- Key event callback ---- */
static void on_key_event(input_event_t event, void *data)
{
    switch (event) {
    case INPUT_EVENT_VOL_DOWN:
        ESP_LOGI(TAG, "KEY: Vol-");
        app_event_fire(APP_EVENT_KEY_VOL_DOWN, NULL);
        break;
    case INPUT_EVENT_VOL_UP:
        ESP_LOGI(TAG, "KEY: Vol+");
        app_event_fire(APP_EVENT_KEY_VOL_UP, NULL);
        break;
    case INPUT_EVENT_PLAY_PAUSE:
        ESP_LOGI(TAG, "KEY: Play/Pause");
        app_event_fire(APP_EVENT_KEY_PLAY_PAUSE, NULL);
        break;
    case INPUT_EVENT_PREV_TRACK:    ESP_LOGI(TAG, "KEY: Prev track");  break;
    case INPUT_EVENT_NEXT_TRACK:    ESP_LOGI(TAG, "KEY: Next track");  break;
    case INPUT_EVENT_SCREEN_TOGGLE: ESP_LOGI(TAG, "KEY: Screen toggle"); break;
    case INPUT_EVENT_POWER_OFF:     ESP_LOGI(TAG, "KEY: Power off");   break;
    }
}

/* ======================================================================== */

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== NomadCast Starting ===");

    /* [1] Power + HW reset */
    gpio_config_t pwr = { .pin_bit_mask = BIT64(PIN_EN_POWER) | BIT64(PIN_LCD_POWER) | BIT64(PIN_BACKLIGHT), .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);    /* 全板外设电源 */
    gpio_set_level(PIN_LCD_POWER, 1);   /* 屏幕电源 */
    gpio_set_level(PIN_BACKLIGHT, 1);   /* 背光 */
    vTaskDelay(pdMS_TO_TICKS(100));
    hw_reset();

    /* [2] Display (ST7789) */
    static esp_lcd_panel_handle_t s_panel = display_init();

    /* [3] LVGL font: copy font_harmony, chain Montserrat as symbol fallback */
    memcpy(&s_font_harmony_with_fb, &font_harmony, sizeof(lv_font_t));
    s_font_harmony_with_fb.fallback = &lv_font_montserrat_14;

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_user_data(disp, s_panel);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    size_t buf_sz = LCD_W * 20 * sizeof(lv_color_t);  /* keep small — SDMMC needs DMA DRAM */
    lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                          LV_THEME_DEFAULT_DARK, g_cjk_font);

    /* [3.5] Boot splash — "NomadCast" logo centered, shown while slow init runs */
    static lv_obj_t *s_boot_splash = NULL;
    {
        s_boot_splash = lv_obj_create(NULL);  /* NULL parent = new screen */
        lv_obj_set_style_bg_color(s_boot_splash, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_boot_splash, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_boot_splash, 0, 0);

        lv_obj_t *logo = lv_label_create(s_boot_splash);
        lv_label_set_text(logo, "NomadCast");
        lv_obj_set_style_text_color(logo, lv_color_white(), 0);
        lv_obj_set_style_text_font(logo, &lv_font_montserrat_20, 0);
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, -12);

        lv_obj_t *sub = lv_label_create(s_boot_splash);
        lv_label_set_text(sub, "Starting...");
        lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 16);

        lv_scr_load(s_boot_splash);

        /* Flush + turn on display now — user sees the logo immediately */
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_lcd_panel_disp_on_off(s_panel, true);
        ESP_LOGI(TAG, "Boot splash shown");
    }

    /* [4] Touch (GT911) — interrupt-driven via drivers/gt911 */
    static lv_indev_t *s_touch_indev = NULL;
    s_gt911_cfg = (gt911_config_t){
        .rst_pin       = PIN_TP_RST,
        .int_pin       = PIN_TP_INT,
        .i2c_sda_pin   = PIN_I2C_SDA,
        .i2c_scl_pin   = PIN_I2C_SCL,
        .i2c_freq_hz   = 100000,
        .max_width     = LCD_W,
        .max_height    = LCD_H,
        .use_interrupt = true,
    };
    if (gt911_init(&s_gt911_cfg, &s_gt911_dev) == ESP_OK) {
        s_touch_indev = lv_indev_create();
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);
        lv_indev_set_scroll_limit(s_touch_indev, 20); /* px; >20px = scroll, not click */
        if (s_gt911_cfg.use_interrupt) {
            gt911_register_isr(s_gt911_dev, on_gt911_touch, NULL);
        }
        /* ES8156 codec shares GT911's software-I2C bus — enable volume control. */
        audio_player_codec_init();
    } else {
        ESP_LOGW(TAG, "Touch not available");
    }

    /* [5] Tick timer */
    esp_timer_create_args_t tick_args = { .callback = lvgl_tick_cb, .arg = NULL, .dispatch_method = ESP_TIMER_TASK, .name = "lv_tick", .skip_unhandled_events = false };
    esp_timer_handle_t tick_h;
    esp_timer_create(&tick_args, &tick_h);
    esp_timer_start_periodic(tick_h, 1000);

    /* [5.5] Global status bar — singleton on lv_layer_top, survives page switches */
    lv_status_bar_init();

    /* [6] Key input */
    input_init(on_key_event, NULL);

    /* [6.5] Sleep monitor — after input is ready, before apps start.
     * Power is never cut during screen-off (SD downloads must survive),
     * so no wake callback is needed — just display+blank + touch disable. */
    sleep_monitor_init(s_panel, s_touch_indev, 0); /* 0 = never sleep (debug) */

    /* [6.6] Flash store — must be before any app reads settings */
    flash_store_init();

    /* [6.7] System clock — starts the 1-second time update timer */
    clock_init();

    /* [6.7b] Battery monitor — 5-min sampler → status-bar icon */
    battery_init();

    /* [6.8] WiFi auto-connect — boot with WiFi on, connect saved network */
    {
        bool wifi_was_on = flash_get_bool("settings", "wifi", true);
        if (wifi_was_on) {
            ESP_LOGI(TAG, "Boot: enabling WiFi…");
            hal_wifi_init();

            hal_wifi_ap_t *nets = NULL;
            int count = hal_wifi_scan(&nets);

            /* Brief delay — the scan API returns immediately, but the WiFi
             * driver needs a moment to sync results into its internal
             * connection cache.  Without this, esp_wifi_connect() logs
             * "Haven't to connect to a suitable AP now!" and fails with
             * transient reason 2 / 4. */
            vTaskDelay(pdMS_TO_TICKS(500));

            wifi_cred_t creds[MAX_WIFI_CREDS];
            int ncreds = wifi_cred_load_all(creds, MAX_WIFI_CREDS);
            for (int i = 0; i < ncreds; i++) {
                bool in_range = false;
                for (int j = 0; j < count; j++) {
                    if (strcmp(nets[j].ssid, creds[i].ssid) == 0) {
                        in_range = true; break;
                    }
                }
                if (!in_range) continue;

                ESP_LOGI(TAG, "Boot: trying '%s'…", creds[i].ssid);

                bool ok = false;
                const char *err = NULL;
                int reason = 0;

                /* Try twice.  The first attempt occasionally fails with
                 * transient reason 2 — the driver's internal scan cache
                 * expires after a disconnect (the connect blocks ~4-5 s
                 * before timing out).  Re-scan before each retry so
                 * hal_wifi_connect() always has fresh AP info. */
                for (int attempt = 0; attempt < 3; attempt++) {
                    hal_wifi_connect(creds[i].ssid, creds[i].password);
                    hal_wifi_get_connect_result(&ok, &err);
                    if (ok) break;
                    reason = hal_wifi_get_disconnect_reason();
                    if (reason == 15 || reason == 202) break; /* bad pwd */
                    if (attempt < 2) {
                        ESP_LOGW(TAG, "Boot: '%s' transient fail (reason %d), re-scanning…",
                                 creds[i].ssid, reason);
                        { /* refresh AP cache before retry */
                            hal_wifi_ap_t *rn = NULL;
                            hal_wifi_scan(&rn);
                            if (rn) free(rn);
                        }
                    }
                }

                if (ok) {
                    ESP_LOGI(TAG, "Boot: connected to '%s'", creds[i].ssid);
                    wifi_cred_save(creds[i].ssid, creds[i].password);
                    break;
                } else {
                    bool bad_pwd = (reason == 15 || reason == 202);
                    ESP_LOGW(TAG, "Boot: '%s' failed (reason %d, %s)%s",
                             creds[i].ssid, reason, err ? err : "?",
                             bad_pwd ? " — deleting" : " — keeping");
                    if (bad_pwd) wifi_cred_delete(creds[i].ssid);
                }
            }
            if (nets) free(nets);
        }
    }

    /* [6.9] OTA — cancel rollback, register event handler */
    ota_init();

    /* [7] App manager + settings + launcher */
    app_manager_init();
    podcast_app_register();
    settings_app_register();

    /* Replace boot splash with launcher home screen */
    if (s_boot_splash) { lv_obj_del(s_boot_splash); s_boot_splash = NULL; }
    launcher_home_ui();
    launcher_gesture_init(s_touch_indev);

    ESP_LOGI(TAG, "=== Ready ===");

    /* Let LVGL render one frame so SPI DMA buffers are allocated before
     * SDMMC claims DMA channels.  Mounting SDMMC later would steal DMA
     * memory and cause lcd_panel.io.spi transmit failures. */
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ── LVGL filesystem callbacks (S: → /sdcard/) ──────────────────────── */

    struct SdFs {
        static void *open_cb(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode) {
            char full[256];
            snprintf(full, sizeof(full), "/sdcard/%s", path);
            return fopen(full, mode == LV_FS_MODE_WR ? "wb" : "rb");
        }
        static lv_fs_res_t close_cb(lv_fs_drv_t *, void *fp) {
            return fclose((FILE *)fp) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
        }
        static lv_fs_res_t read_cb(lv_fs_drv_t *, void *fp, void *buf, uint32_t btr, uint32_t *br) {
            *br = (uint32_t)fread(buf, 1, btr, (FILE *)fp);
            return (*br > 0 || btr == 0) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
        }
        static lv_fs_res_t seek_cb(lv_fs_drv_t *, void *fp, uint32_t pos, lv_fs_whence_t w) {
            int wh = (w == LV_FS_SEEK_SET) ? SEEK_SET : (w == LV_FS_SEEK_CUR) ? SEEK_CUR : SEEK_END;
            return fseek((FILE *)fp, (long)pos, wh) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
        }
        static lv_fs_res_t tell_cb(lv_fs_drv_t *, void *fp, uint32_t *pos) {
            long p = ftell((FILE *)fp);
            *pos = (uint32_t)(p >= 0 ? p : 0);
            return p >= 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
        }
    };

    /* [7.5] SD card — deferred mount so SPI LCD already owns its DMA buffers */
    {
#define SD_MOUNT_POINT "/sdcard"
        sdmmc_host_t sd_host = SDMMC_HOST_DEFAULT();
        sd_host.flags = SDMMC_HOST_FLAG_1BIT;
        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_cfg.clk   = NOMADCAST_PIN_SD_CLK;    /* GPIO10 */
        slot_cfg.cmd   = NOMADCAST_PIN_SD_CMD;    /* GPIO11 */
        slot_cfg.d0    = NOMADCAST_PIN_SD_DAT0;   /* GPIO9 */
        slot_cfg.width = 1;
        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 0,
            .disk_status_check_enable = false,
            .use_one_fat = false,
        };
        sdmmc_card_t *sd_card = NULL;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &sd_host,
            &slot_cfg, &mount_cfg, &sd_card);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);

            /* Register LVGL FS driver (S:) → prepend /sdcard/.
             * Plain C callbacks — no lambdas, reliable on ESP-IDF. */
            static lv_fs_drv_t s_fs_drv;
            lv_fs_drv_init(&s_fs_drv);
            s_fs_drv.letter = 'S';
            s_fs_drv.cache_size = 4096;
            s_fs_drv.open_cb  = SdFs::open_cb;
            s_fs_drv.close_cb = SdFs::close_cb;
            s_fs_drv.read_cb  = SdFs::read_cb;
            s_fs_drv.seek_cb  = SdFs::seek_cb;
            s_fs_drv.tell_cb  = SdFs::tell_cb;
            lv_fs_drv_register(&s_fs_drv);
            ESP_LOGI(TAG, "LVGL FS driver (S:) → /sdcard/");

            /* Initialize logging system — SD card is ready */
            log_system_init();
        } else {
            ESP_LOGW(TAG, "SD card not available (%s), downloads disabled",
                     esp_err_to_name(ret));
        }
    }

    /* [8] LVGL loop */
    while (1) {
        /* Process pending async operations (RSS feed, downloads) */
        controller_process_rss();
        controller_process_download();

        uint32_t delay = lv_timer_handler();
        vTaskDelay(delay > 0 ? pdMS_TO_TICKS(delay) : 1);
    }
}
