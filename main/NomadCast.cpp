/*
 * NomadCast — Main Entry Point
 *
 * ESP32-S3 PCB V1.2 (V2 profile). Display settings verified in t_display:
 *   1. Power ON → HW reset → Display (ST7789) → Touch (CST836U) → LVGL
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
#include "nomadcast_display.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

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
#include "alarm_service.h"
#include "flash_store.h"
#include "clock.h"
#include "lang.h"
#include "battery.h"
#include "audio_player.h"
#include "ota.h"
#include "touch_controller.h"
#include "display_controller.h"
#include "usb_storage.h"
#include "nomadcast.h"
#include "hal.h"
#include "wifi_cred.h"
#include "log_system.h"
#include "log_uploader.h"
#include "monitor.h"

/* SD card */
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
void settings_app_register(void);
	void podcast_app_register(void);
	void radio_app_register(void);
	void player_app_register(void);
	void alarm_app_register(void);
	void weather_app_register(void);
	void controller_process_rss(void);
	void controller_process_download(void);
    void podcast_controller_suspend_storage(void);
    void podcast_controller_restore_storage(void);
}

#include "esp_check.h"  /* ESP_RETURN_ON_ERROR */

static const char *TAG = "NomadCast";

/* CJK font with Montserrat fallback for LV_SYMBOL_* glyphs */
extern "C" const struct _lv_font_t font_harmony;
static lv_font_t s_font_harmony_with_fb;  /* mutable copy — allows fallback chaining */
/* g_cjk_font defined with C linkage (used by lv_page.c etc.) */
extern "C" { const lv_font_t *g_cjk_font = &s_font_harmony_with_fb; }

/* ---- Pin map (PCB V1.2 — from nomadcast.h) ---- */
#define PIN_EN_POWER    NOMADCAST_PIN_AP_POWER
#define PIN_BACKLIGHT   NOMADCAST_PIN_LCD_BL
#define PIN_I2C_SDA     NOMADCAST_PIN_I2C_SDA
#define PIN_I2C_SCL     NOMADCAST_PIN_I2C_SCL
#define PIN_TP_INT      NOMADCAST_PIN_TP_INT
#define PIN_TP_RST      NOMADCAST_PIN_TP_RST
#define LCD_W           240
#define LCD_H           320
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (60 * 1000 * 1000)

/* ---- Display (ST7789 via SPI2, esp_lcd) — proven from t_ui ---- */
static esp_lcd_panel_handle_t display_init(void)
{
    spi_bus_config_t spi_cfg = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1,
        .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,  /* match flush buffer height */
        .flags = 0, .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO, .intr_flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = SPI_FREQ_HZ, .trans_queue_depth = 10,
        .on_color_trans_done = NULL, .user_ctx = NULL,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .flags = {},
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
    gpio_config_t cfg = { .pin_bit_mask = BIT64(PIN_TP_RST), .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* INT is input only; CST836U needs no address-selection sequence. */
    gpio_config_t in_cfg = { .pin_bit_mask = BIT64(PIN_TP_INT), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_TP_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(PIN_TP_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
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

/* ---- Long-press power key → deep-sleep shutdown ---- */
static void power_off(void)
{
    ESP_LOGI(TAG, "Power off (long-press)");

    /* Stop audio and flush logs so the SD card is left consistent. */
    audio_player_stop();
    log_system_shutdown();

    /* NOTE: don't cut PIN_EN_POWER (GPIO46) here — it may power the USB/UART
     * bridge and break download-mode auto-reset. Deep sleep already powers
     * down the peripherals, so cutting it separately is unnecessary. */
    // gpio_set_level(PIN_EN_POWER, 0);

    /* Power off immediately when the runtime long-press event fires — no wait
     * for release. If the key is still held (HIGH), the EXT1 wake fires right
     * away; app_main then applies the separate power-on hold threshold. */

    /* 配置 GPIO9(WAKEUP_MCU) 为输入+下拉,以便读取当前 USB 插入状态 */
    {
        gpio_config_t usb_cfg = {
            .pin_bit_mask = BIT64(NOMADCAST_PIN_USB_WAKEUP),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&usb_cfg);
    }

    /* 唤醒源:电源键(GPIO5) + WAKEUP_MCU(GPIO9, 仅当 USB 当前未插入)。
     * 若 USB 已插着(GPIO9 已为高),不把 GPIO9 加入唤醒源,否则进深睡后会立即唤醒。 */
    uint64_t wake_mask = BIT64(NOMADCAST_PIN_KEY_POWER);
    if (!gpio_get_level(NOMADCAST_PIN_USB_WAKEUP)) {
        wake_mask |= BIT64(NOMADCAST_PIN_USB_WAKEUP);
        rtc_gpio_pulldown_en(NOMADCAST_PIN_USB_WAKEUP);
        rtc_gpio_pullup_dis(NOMADCAST_PIN_USB_WAKEUP);
    }
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

    /* Keep the power key pulled LOW in deep sleep (RTC pull survives sleep),
     * so it only wakes on a real press (HIGH), not a floating pin. Must be
     * configured AFTER the wake source above. */
    rtc_gpio_pulldown_en(NOMADCAST_PIN_KEY_POWER);
    rtc_gpio_pullup_dis(NOMADCAST_PIN_KEY_POWER);

    /* Arm an RTC timer so an upcoming alarm wakes the device out of deep sleep. */
    alarm_service_prepare_sleep();

    ESP_LOGI(TAG, "Entering deep sleep now...");
    esp_deep_sleep_start();
}

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
    case INPUT_EVENT_POWER_OFF:     power_off(); break;
    }
}

/* ======================================================================== */

/* ---- 关机态唤醒后：检测是否持续按住电源键达到开机阈值 ---- */
static void check_power_on_hold(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return;   /* 非 EXT1 唤醒(如复位/USB-OTG)，直接返回 */
    }

    /* WAKEUP_MCU(GPIO9) 插入唤醒 → 直接开机(亮屏由正常启动流程处理)，无需长按确认 */
    if (esp_sleep_get_ext1_wakeup_status() & BIT64(NOMADCAST_PIN_USB_WAKEUP)) {
        ESP_LOGI(TAG, "Boot via USB plug-in");
        rtc_gpio_deinit(NOMADCAST_PIN_USB_WAKEUP);
        return;
    }

    /* 重新配置 GPIO5 为普通输入（唤醒后由 RTC 模式恢复） */
    rtc_gpio_deinit(NOMADCAST_PIN_KEY_POWER);
    gpio_config_t key_cfg = {
        .pin_bit_mask = BIT64(NOMADCAST_PIN_KEY_POWER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key_cfg);

    /* 循环检测 POWER_ON_LONG_PRESS_MS，看用户是否持续按住 */
    bool long_pressed = true;
    int check_ticks = POWER_ON_LONG_PRESS_MS / 10;
    while (check_ticks-- > 0) {
        if (!gpio_get_level(NOMADCAST_PIN_KEY_POWER)) {
            long_pressed = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* 没按够开机阈值（短按误触）→ 重新打回深睡，保持关机状态 */
    if (!long_pressed) {
        ESP_LOGI(TAG, "Boot canceled: key not held %d ms, returning to sleep",
                 POWER_ON_LONG_PRESS_MS);
        esp_sleep_enable_ext1_wakeup(BIT64(NOMADCAST_PIN_KEY_POWER), ESP_EXT1_WAKEUP_ANY_HIGH);
        rtc_gpio_pulldown_en(NOMADCAST_PIN_KEY_POWER);
        rtc_gpio_pullup_dis(NOMADCAST_PIN_KEY_POWER);
        esp_deep_sleep_start();
    }

    ESP_LOGI(TAG, "Power-on long press confirmed (%d ms), booting immediately...",
             POWER_ON_LONG_PRESS_MS);
    /* 不再死等松手——开机免疫期(见 input.c)会吞掉松手时的误触短按 */
}

/* ---- Application startup orchestration ---- */

typedef struct {
    esp_lcd_panel_handle_t panel;
    lv_indev_t            *touch_indev;
    lv_obj_t              *boot_splash;
} app_context_t;

static void board_power_init(void)
{
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(PIN_EN_POWER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);    /* 全板外设电源 */
    nomadcast_display_power_on();
    hw_reset();
}

static void lvgl_display_init(esp_lcd_panel_handle_t panel)
{
    memcpy(&s_font_harmony_with_fb, &font_harmony, sizeof(lv_font_t));
    s_font_harmony_with_fb.fallback = &lv_font_montserrat_14;

    lv_init();
    lv_display_t *display = lv_display_create(LCD_W, LCD_H);
    lv_display_set_user_data(display, panel);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    size_t buf_sz = LCD_W * 40 * sizeof(lv_color_t);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
        buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
        buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_display_set_buffers(display, buf1, buf2, buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_theme_default_init(display,
                          lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_RED),
                          LV_THEME_DEFAULT_DARK,
                          g_cjk_font);
}

static lv_obj_t *boot_splash_show(void)
{
    lv_obj_t *splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash, 0, 0);

    lv_obj_t *logo = lv_label_create(splash);
    lv_label_set_text(logo, "NomadCast");
    lv_obj_set_style_text_color(logo, lv_color_white(), 0);
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_20, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *subtitle = lv_label_create(splash);
    lv_label_set_text(subtitle, "Starting...");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 16);

    lv_scr_load(splash);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(display_controller_wake());
    ESP_LOGI(TAG, "Boot splash shown");
    return splash;
}

static lv_indev_t *app_touch_init(void)
{
    static i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t config = {};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = PIN_I2C_SDA;
    config.scl_io_num = PIN_I2C_SCL;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared I2C bus unavailable: %s", esp_err_to_name(err));
        return NULL;
    }
    lv_indev_t *touch_indev = NULL;
    err = touch_controller_init(bus, lv_display_get_default(), &touch_indev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST836U unavailable: %s", esp_err_to_name(err));
    }
    /* ES8156 remains available even if touch initialization fails. */
    audio_player_codec_init(bus);
    return touch_indev;
}

static void lvgl_tick_start(void)
{
    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_tick",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t tick_handle;
    esp_timer_create(&tick_args, &tick_handle);
    esp_timer_start_periodic(tick_handle, 1000);
}

static void system_services_init(const app_context_t *app)
{
    lv_status_bar_init();
    input_init(on_key_event, NULL);

    /* Keep peripheral power on during screen-off so downloads can continue. */
    sleep_monitor_init(app->panel, app->touch_indev, 0);
    sleep_monitor_set_backlight_pin(PIN_BACKLIGHT);

    /* Settings consumers depend on flash_store being initialized first. */
    flash_store_init();
    lang_init();
    clock_init();
    alarm_service_init();
    battery_init();

    /* Auto power-off: default 15 min idle, persisted by Settings → General.
     * The action is the same deep-sleep shutdown as a long-press power key.
     * The safety gate (registered by the podcast controller) keeps it from
     * firing while playback or a download is active. */
    sleep_monitor_set_auto_power_off_timeout(flash_get_i32("settings", "auto_power_off", 15));
    sleep_monitor_set_power_off_action(power_off);
}

static bool wifi_ssid_is_visible(const char *ssid,
                                 const hal_wifi_ap_t *networks,
                                 int network_count)
{
    for (int i = 0; i < network_count; i++) {
        if (strcmp(networks[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

static bool wifi_connect_saved_credential(const wifi_cred_t *cred)
{
    ESP_LOGI(TAG, "Boot: trying '%s'…", cred->ssid);

    bool connected = false;
    const char *error = NULL;
    int reason = 0;

    /* Re-scan between transient failures so each retry has fresh AP data. */
    for (int attempt = 0; attempt < 3; attempt++) {
        hal_wifi_connect(cred->ssid, cred->password);
        hal_wifi_get_connect_result(&connected, &error);
        if (connected) break;

        reason = hal_wifi_get_disconnect_reason();
        if (reason == 15 || reason == 202) break; /* bad password */

        if (attempt < 2) {
            ESP_LOGW(TAG, "Boot: '%s' transient fail (reason %d), re-scanning…",
                     cred->ssid, reason);
            hal_wifi_ap_t *refreshed_networks = NULL;
            hal_wifi_scan(&refreshed_networks);
            if (refreshed_networks) free(refreshed_networks);
        }
    }

    if (connected) {
        ESP_LOGI(TAG, "Boot: connected to '%s'", cred->ssid);
        wifi_cred_save(cred->ssid, cred->password);
        return true;
    }

    bool bad_password = (reason == 15 || reason == 202);
    ESP_LOGW(TAG, "Boot: '%s' failed (reason %d, %s)%s",
             cred->ssid, reason, error ? error : "?",
             bad_password ? " — deleting" : " — keeping");
    if (bad_password) wifi_cred_delete(cred->ssid);
    return false;
}

static void wifi_auto_connect(void)
{
    if (!flash_get_bool("settings", "wifi", true)) {
        return;
    }

    ESP_LOGI(TAG, "Boot: enabling WiFi…");
    hal_wifi_init();

    hal_wifi_ap_t *networks = NULL;
    int network_count = hal_wifi_scan(&networks);

    /* Give the driver time to sync scan results into its connection cache. */
    vTaskDelay(pdMS_TO_TICKS(500));

    wifi_cred_t credentials[MAX_WIFI_CREDS];
    int credential_count = wifi_cred_load_all(credentials, MAX_WIFI_CREDS);
    for (int i = 0; i < credential_count; i++) {
        if (!wifi_ssid_is_visible(credentials[i].ssid, networks, network_count)) {
            continue;
        }
        if (wifi_connect_saved_credential(&credentials[i])) {
            break;
        }
    }

    if (networks) free(networks);
}

static void ota_services_init(void)
{
    ota_init();

    /* Auto-update is opt-in.  The settings namespace is cleared by factory
     * reset, so the single persisted flag remains the source of truth. */
    bool auto_update = flash_get_bool("settings", "autoup", false);
    if (auto_update && hal_wifi_is_connected()) {
        char update_url[256];
        flash_get_str("settings", "upd_url", update_url, sizeof(update_url), "");
        ota_auto_update_check(update_url[0] ? update_url : OTA_DEFAULT_MANIFEST_URL);
    }
}

static void launcher_start(app_context_t *app)
{
    app_manager_init();
    podcast_app_register();
    radio_app_register();
    player_app_register();
    weather_app_register();
    alarm_app_register();
    settings_app_register();

    if (app->boot_splash) {
        lv_obj_del(app->boot_splash);
        app->boot_splash = NULL;
    }
    launcher_home_ui();
    launcher_gesture_init(app->touch_indev);
    ESP_LOGI(TAG, "=== Ready ===");
}

static void lvgl_prepare_for_storage(void)
{
    /* Render once before SDMMC claims DMA resources. */
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50));
}

#define SD_MOUNT_POINT "/sdcard"

static void *sd_fs_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    char full_path[256];
    snprintf(full_path, sizeof(full_path), SD_MOUNT_POINT "/%s", path);
    return fopen(full_path, mode == LV_FS_MODE_WR ? "wb" : "rb");
}

static lv_fs_res_t sd_fs_close(lv_fs_drv_t *, void *file)
{
    return fclose((FILE *)file) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_fs_read(lv_fs_drv_t *, void *file, void *buffer,
                              uint32_t bytes_to_read, uint32_t *bytes_read)
{
    *bytes_read = (uint32_t)fread(buffer, 1, bytes_to_read, (FILE *)file);
    return (*bytes_read > 0 || bytes_to_read == 0)
               ? LV_FS_RES_OK
               : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_fs_seek(lv_fs_drv_t *, void *file, uint32_t position,
                              lv_fs_whence_t whence)
{
    int origin = (whence == LV_FS_SEEK_SET)
                     ? SEEK_SET
                     : (whence == LV_FS_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    return fseek((FILE *)file, (long)position, origin) == 0
               ? LV_FS_RES_OK
               : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t sd_fs_tell(lv_fs_drv_t *, void *file, uint32_t *position)
{
    long current_position = ftell((FILE *)file);
    *position = (uint32_t)(current_position >= 0 ? current_position : 0);
    return current_position >= 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static void lvgl_sd_filesystem_register(void)
{
    static lv_fs_drv_t fs_driver;
    lv_fs_drv_init(&fs_driver);
    fs_driver.letter = 'S';
    fs_driver.cache_size = 4096;
    fs_driver.open_cb  = sd_fs_open;
    fs_driver.close_cb = sd_fs_close;
    fs_driver.read_cb  = sd_fs_read;
    fs_driver.seek_cb  = sd_fs_seek;
    fs_driver.tell_cb  = sd_fs_tell;
    lv_fs_drv_register(&fs_driver);
    ESP_LOGI(TAG, "LVGL FS driver (S:) → " SD_MOUNT_POINT "/");
}

static void storage_init(void)
{
    esp_err_t result = usb_storage_mount();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), downloads disabled",
                 esp_err_to_name(result));
        return;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    lvgl_sd_filesystem_register();
    log_system_init();
}

static void main_event_loop(void)
{
    while (1) {
        usb_storage_process();
        if (usb_storage_enabled() || usb_storage_busy()) sleep_monitor_notify_activity();
        app_event_process();
        controller_process_download();
        audio_player_process();
        alarm_service_process();

        uint32_t delay = lv_timer_handler();
        vTaskDelay(delay > 0 ? pdMS_TO_TICKS(delay) : 1);
    }
}

static void storage_prepare_usb(void)
{
    podcast_controller_suspend_storage();
    audio_player_stop();
    log_uploader_pause(true);
    log_system_shutdown();
    lv_image_cache_drop(NULL);
    lv_image_header_cache_drop(NULL);
}

static void storage_resume_local(void)
{
    lv_image_cache_drop(NULL);
    lv_image_header_cache_drop(NULL);
    log_system_init();
    log_uploader_pause(false);
    if (hal_wifi_is_connected()) log_uploader_on_wifi_connected();
    podcast_controller_restore_storage();
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== NomadCast Starting ===");

    app_context_t app = {};

    check_power_on_hold();
    board_power_init();
    ESP_ERROR_CHECK(usb_storage_init());
    app.panel = nomadcast_display_init(LCD_W * 20 * 2);
    ESP_ERROR_CHECK(display_controller_init(app.panel, PIN_BACKLIGHT));

    lvgl_display_init(app.panel);
    app.boot_splash = boot_splash_show();

    app.touch_indev = app_touch_init();
    lvgl_tick_start();
    system_services_init(&app);

    wifi_auto_connect();

    ota_services_init();
    launcher_start(&app);
    monitor_init();

    lvgl_prepare_for_storage();

    storage_init();
    usb_storage_set_callbacks(storage_prepare_usb, storage_resume_local);

    main_event_loop();
}
