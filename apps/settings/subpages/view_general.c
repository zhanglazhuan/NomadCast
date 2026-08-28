/**
 * @file view_general.c
 * @brief General 设置页 — 时区 / 语言 / 时间格式 (12h/24h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "view_general.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "lv_page.h"
#include "lv_bottom_sheet.h"
#include "sleep_monitor.h"
#include "flash_store.h"
#include "app_manager.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern SettingsApp g_settings_app;
static const char *TAG = "settings_reset";
static lv_bottom_sheet_t *s_factory_reset_sheet = NULL;

/* ── 时间格式 switch 回调 ──────────────────────────────────────────────────── */

static void on_time_format_switch(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool fmt24 = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_model_set_time_format_24h(&g_settings_app, fmt24);
}

/* ── 时区变更回调 ──────────────────────────────────────────────────────────── */

static void on_timezone_changed(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    settings_model_set_timezone(&g_settings_app, sel);
}

/* ── 睡眠超时选择回调 ────────────────────────────────────────────────────── */

/* Dropdown index → minutes */
static const int _sleep_timeout_values[] = {0, 1, 2, 5, 10, 15, 30, 60};

static void on_sleep_timeout_changed(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    if (sel >= 0 && sel < (int)(sizeof(_sleep_timeout_values) / sizeof(_sleep_timeout_values[0]))) {
        int minutes = _sleep_timeout_values[sel];
        settings_model_set_sleep_timeout(&g_settings_app, minutes);
        sleep_monitor_set_timeout(minutes);
    }
}

/* ── 自动关机超时选择回调 ────────────────────────────────────────────────── */

/* Dropdown index → minutes (0 = never) */
static const int _power_off_values[] = {0, 5, 10, 15, 30, 60};

static void on_power_off_timeout_changed(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    if (sel >= 0 && sel < (int)(sizeof(_power_off_values) / sizeof(_power_off_values[0]))) {
        int minutes = _power_off_values[sel];
        settings_model_set_auto_power_off(&g_settings_app, minutes);
    }
}

/* ── 语言变更回调 ──────────────────────────────────────────────────────────── */

static void on_language_changed(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    settings_model_set_language(&g_settings_app, sel);
}

/* ── 辅助: 创建一个设置行 ────────────────────────────────────────────────── */

static lv_obj_t* create_setting_row(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

    if (label) {
        lv_obj_t* lb = lv_label_create(row);
        lv_label_set_text(lb, label);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x666666), 0);
        lv_obj_set_style_margin_bottom(lb, 4, 0);
    }
    return row;
}

/* ── 恢复出厂设置 ────────────────────────────────────────────────────────── */

static void remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *ent;
    char child[256];
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n > 0 && n < (int)sizeof(child)) remove_tree(child);
    }
    closedir(dir);
    rmdir(path);
}

static void factory_reset_task(void *arg)
{
    (void)arg;
    /* The user partition is the only NVS partition erased here; nvs (system)
     * remains intact. SD content is best-effort and bounded to this directory. */
    bool apps_ok = app_manager_factory_reset_all();
    if (!apps_ok) ESP_LOGE(TAG, "one or more app reset callbacks failed");
    remove_tree("/sdcard/.nomadcast");
    ESP_LOGI(TAG, "factory reset complete; restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    vTaskDelete(NULL);
}

static void on_factory_reset_confirm(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "factory reset confirmed");
    if (s_factory_reset_sheet) {
        lv_bottom_sheet_close(s_factory_reset_sheet);
        s_factory_reset_sheet = NULL;
    }
    if (xTaskCreate(factory_reset_task, "factory_reset", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start factory reset task");
    }
}

static void on_factory_reset_cancel(lv_event_t *e)
{
    (void)e;
    if (s_factory_reset_sheet) {
        lv_bottom_sheet_close(s_factory_reset_sheet);
        s_factory_reset_sheet = NULL;
    }
}

static void on_factory_reset_clicked(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "factory reset confirmation requested");
    if (s_factory_reset_sheet) return;

    s_factory_reset_sheet = lv_bottom_sheet_create(lv_screen_active());
    if (!s_factory_reset_sheet) {
        ESP_LOGE(TAG, "failed to create factory reset confirmation sheet");
        return;
    }

    lv_obj_t *content = lv_bottom_sheet_get_content(s_factory_reset_sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 16, 0);
    lv_obj_set_style_pad_row(content, 10, 0);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Factory Reset");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *warn = lv_label_create(content);
    lv_label_set_text(warn, "Erase all settings and downloaded data?\nThe device will restart.");
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);

    lv_obj_t *confirm = lv_button_create(content);
    lv_obj_set_size(confirm, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xF44336), 0);
    lv_obj_add_event_cb(confirm, on_factory_reset_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(confirm);
    lv_label_set_text(clbl, "Erase and Restart");
    lv_obj_center(clbl);
    lv_obj_set_style_text_color(clbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, LV_PCT(100), 44);
    lv_obj_add_event_cb(cancel, on_factory_reset_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(cancel);
    lv_label_set_text(x, "Cancel");
    lv_obj_center(x);
}

/* ── 构建 General 页面 ──────────────────────────────────────────────────────── */

static lv_obj_t* build_general_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    Page page = lv_page_create("General", true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(cont, 12, 0);
    lv_obj_set_style_pad_ver(cont, 4, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);

    int tz = settings_model_get_timezone(app);
    int lang = settings_model_get_language(app);
    bool fmt24 = settings_model_get_time_format_24h(app);

    /* ── 时区 ── */
    lv_obj_t* row_tz = create_setting_row(cont, "Timezone");
    lv_obj_t* dd_tz = lv_dropdown_create(row_tz);
    lv_dropdown_set_options(dd_tz, settings_timezone_options);
    lv_dropdown_set_selected(dd_tz, tz);
    lv_obj_set_width(dd_tz, LV_PCT(100));
    lv_obj_add_event_cb(dd_tz, on_timezone_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── 语言 ── */
    lv_obj_t* row_lang = create_setting_row(cont, "Language");
    lv_obj_t* dd_lang = lv_dropdown_create(row_lang);
    lv_dropdown_set_options(dd_lang, settings_language_options);
    lv_dropdown_set_selected(dd_lang, lang);
    lv_obj_set_width(dd_lang, LV_PCT(100));
    lv_obj_add_event_cb(dd_lang, on_language_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── 时间格式 ── */
    lv_obj_t* row_fmt = create_setting_row(cont, "24-Hour Format");
    lv_obj_t* sw_fmt = lv_switch_create(row_fmt);
    lv_obj_set_height(sw_fmt, 24);
    if (fmt24) lv_obj_add_state(sw_fmt, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_fmt, on_time_format_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── 睡眠超时 ── */
    int cur_timeout = settings_model_get_sleep_timeout(app);
    int dd_idx = 0;
    for (int i = 0; i < (int)(sizeof(_sleep_timeout_values) / sizeof(_sleep_timeout_values[0])); i++) {
        if (_sleep_timeout_values[i] == cur_timeout) { dd_idx = i; break; }
    }

    lv_obj_t* row_sleep = create_setting_row(cont, "Sleep Timeout");
    lv_obj_t* dd_sleep = lv_dropdown_create(row_sleep);
    lv_dropdown_set_options(dd_sleep, "Never\n1 Minute\n2 Minutes\n5 Minutes\n10 Minutes\n15 Minutes\n30 Minutes\n60 Minutes");
    lv_dropdown_set_selected(dd_sleep, dd_idx);
    lv_obj_set_width(dd_sleep, LV_PCT(100));
    lv_obj_add_event_cb(dd_sleep, on_sleep_timeout_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── 自动关机超时 ── */
    int cur_power_off = settings_model_get_auto_power_off(app);
    int po_idx = 0;
    for (int i = 0; i < (int)(sizeof(_power_off_values) / sizeof(_power_off_values[0])); i++) {
        if (_power_off_values[i] == cur_power_off) { po_idx = i; break; }
    }

    lv_obj_t* row_power = create_setting_row(cont, "Auto Power Off");
    lv_obj_t* dd_power = lv_dropdown_create(row_power);
    lv_dropdown_set_options(dd_power, "Never\n5 Minutes\n10 Minutes\n15 Minutes\n30 Minutes\n60 Minutes");
    lv_dropdown_set_selected(dd_power, po_idx);
    lv_obj_set_width(dd_power, LV_PCT(100));
    lv_obj_add_event_cb(dd_power, on_power_off_timeout_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── 恢复出厂设置 ── */
    lv_obj_t *row_reset = create_setting_row(cont, NULL);
    lv_obj_set_style_bg_opa(row_reset, LV_OPA_TRANSP, 0);
    lv_obj_t *btn_reset = lv_button_create(row_reset);
    lv_obj_set_size(btn_reset, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_radius(btn_reset, 8, 0);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "Factory Reset");
    lv_obj_set_style_text_color(lbl_reset, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_reset);
    lv_obj_add_event_cb(btn_reset, on_factory_reset_clicked, LV_EVENT_CLICKED, NULL);

    return page.screen;
}

void settings_view_general_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_GENERAL, build_general_page);
}
