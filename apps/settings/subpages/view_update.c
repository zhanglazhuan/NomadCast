/**
 * @file view_update.c
 * @brief Update settings — auto-update toggle + check for update button (real OTA)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_update.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "hal.h"
#include "lv_page.h"
#include "lv_toast.h"
#include "lv_bottom_sheet.h"
#include "view_ota_status.h"
#include "ota.h"
#include "flash_store.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern SettingsApp g_settings_app;

/* Default manifest URL — flash key "upd_url" can override it (see ota.h). */

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static const char *get_manifest_url(void) {
    static char url[256];
    flash_get_str("settings", "upd_url", url, sizeof(url), "");
    if (url[0] == '\0') {
        /* No URL configured — use default */
        return OTA_DEFAULT_MANIFEST_URL;
    }
    return url;
}

static bool url_looks_valid(const char *url) {
    if (!url || url[0] == '\0') return false;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

/* Render changelog as up to `max_lines` single-line labels. Each item stays on
 * one line (ellipsis-truncated), never wrapping — so the card height is fixed. */
static void add_changelog_lines(lv_obj_t *parent, const char *changelog, int max_lines) {
    if (!changelog || !changelog[0]) return;

    char buf[256];
    strncpy(buf, changelog, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int shown = 0;
    char *line = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (*line && shown < max_lines) {
                lv_obj_t *lbl = lv_label_create(parent);
                lv_label_set_text(lbl, line);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
                shown++;
            }
            line = p + 1;
        }
    }
    if (*line && shown < max_lines) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, line);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    }
}

/* ── Toast wrappers ─────────────────────────────────────────────────────── */

static void toast(const char *msg) { lv_toast_show(msg, 3000); }

/* ── Auto update switch ───────────────────────────────────────────────────── */

static void on_auto_update_switch(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_model_set_auto_update(&g_settings_app, on);
}

/* ── OTA check callback ───────────────────────────────────────────────────── */

/* ── Firmware URL preserved across OTA check → install flow ─────────────── */
static char s_firmware_url[512];
/* ── Container holding inline update cards (created in build_update_page) ── */
static lv_obj_t *s_cards_container = NULL;

static void on_ota_progress(int percent, void *user_data)
{
    (void)user_data;
    /* Runs on the esp_event loop task (sys_evt), NOT the LVGL thread — just
     * record the value; the OTA Status page's poll timer renders it. */
    g_ota_progress = percent;
}

static void ota_worker_task(void *arg)
{
    (void)arg;
    bool ok = ota_perform(s_firmware_url, on_ota_progress, NULL);
    if (ok) {
        g_ota_progress = 100;
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        g_ota_progress = -2;   /* signal the OTA Status page to show failure */
    }
    vTaskDelete(NULL);
}

static lv_bottom_sheet_t *s_ota_sheet = NULL;

static void on_confirm_ota(lv_event_t *e)
{
    (void)e;
    if (s_ota_sheet) { lv_bottom_sheet_close(s_ota_sheet); s_ota_sheet = NULL; }

    g_ota_progress = -1;
    PAGE_NAVIGATE_TO((&g_settings_app), SETTINGS_PAGE_UPDATE, SETTINGS_PAGE_OTA_STATUS, NULL);

    /* Run OTA in a separate task so the LVGL thread stays free to refresh the
     * progress bar. ota_perform() blocks until download+flash completes. */
    xTaskCreate(ota_worker_task, "ota_work", 8192, NULL, 5, NULL);
}

static void on_cancel_ota(lv_event_t *e)
{
    (void)e;
    if (s_ota_sheet) { lv_bottom_sheet_close(s_ota_sheet); s_ota_sheet = NULL; }
}

static void on_install_clicked(lv_event_t *e)
{
    (void)e;
    /* Show a bottom sheet with OTA precautions, then confirm to proceed. */
    lv_obj_t *scr = lv_screen_active();
    s_ota_sheet = lv_bottom_sheet_create(scr);

    lv_obj_t *content = lv_bottom_sheet_get_content(s_ota_sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 16, 0);
    lv_obj_set_style_pad_row(content, 12, 0);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "OTA Update");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *warn = lv_label_create(content);
    lv_label_set_text(warn, "Battery must be at least 30%.\nDo not power off during the update.");
    lv_obj_set_style_text_color(warn, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);

    lv_obj_t *confirm = lv_button_create(content);
    lv_obj_set_size(confirm, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_radius(confirm, 6, 0);
    lv_obj_add_event_cb(confirm, on_confirm_ota, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cfl = lv_label_create(confirm);
    lv_label_set_text(cfl, "Confirm");
    lv_obj_center(cfl);
    lv_obj_set_style_text_color(cfl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *cancel = lv_button_create(content);
    lv_obj_set_size(cancel, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_add_event_cb(cancel, on_cancel_ota, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ccl = lv_label_create(cancel);
    lv_label_set_text(ccl, "Cancel");
    lv_obj_center(ccl);
}

static void on_ota_checked(ota_check_result_t result,
                            const char *new_version,
                            const char *changelog,
                            const char *firmware_url,
                            const char *release_date,
                            void *user_data)
{
    (void)user_data;

    switch (result) {
    case OTA_CHECK_UP_TO_DATE:
        toast("Already up-to-date");
        break;

    case OTA_CHECK_UPDATE_AVAILABLE: {
        if (firmware_url) snprintf(s_firmware_url, sizeof(s_firmware_url), "%s", firmware_url);
        if (!s_cards_container || !lv_obj_is_valid(s_cards_container)) break;

        /* Clear any previous update cards, then show the newest one inline. */
        lv_obj_clean(s_cards_container);

        lv_obj_t *card = lv_obj_create(s_cards_container);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);       /* 改为内容自适应高度，紧凑显示，避免留白和截断按钮 */
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);                 /* 灰色边框 — 标记"新版本"区域 */
        lv_obj_set_style_border_color(card, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);           /* 上下排:信息在上,按钮在底 */
        lv_obj_set_style_pad_row(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);           /* 不要竖向滚动 */
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        /* 信息区:版本名 + 上线日期 + changelog(最多3条) */
        lv_obj_t *info = lv_obj_create(card);
        lv_obj_set_width(info, LV_PCT(100));
        lv_obj_set_height(info, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(info, 4, 0);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);

        /* 标题直接用版本名 */
        lv_obj_t *title = lv_label_create(info);
        lv_label_set_text(title, new_version ? new_version : "");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

        if (release_date && release_date[0]) {
            lv_obj_t *date = lv_label_create(info);
            lv_label_set_text_fmt(date, "Released: %s", release_date);
            lv_obj_set_style_text_color(date, lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_font(date, &lv_font_montserrat_12, 0);
        }

        /* 最多显示3条，单行截断不跨行 */
        add_changelog_lines(info, changelog, 3);

        /* 底部:OTA Now 按钮 (整行宽) */
        lv_obj_t *install = lv_button_create(card);
        lv_obj_set_size(install, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(install, lv_color_hex(0x4CAF50), 0);
        lv_obj_set_style_radius(install, 6, 0);
        lv_obj_t *in_lb = lv_label_create(install);
        lv_label_set_text(in_lb, "OTA Now");
        lv_obj_center(in_lb);
        lv_obj_set_style_text_color(in_lb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(in_lb, &lv_font_montserrat_14, 0);
        lv_obj_add_event_cb(install, on_install_clicked, LV_EVENT_CLICKED, NULL);
        break;
    }

    case OTA_CHECK_ERROR_NO_URL:
        toast("URL not configured");
        break;

    case OTA_CHECK_ERROR_NETWORK:
        toast("Server unreachable");
        break;

    case OTA_CHECK_ERROR_PARSE:
        toast("Invalid server response");
        break;
    }
}

/* ── Check update button ──────────────────────────────────────────────────── */

static void on_check_update_clicked(lv_event_t *e) {
    (void)e;

    /* Pre-checks — fail fast with specific messages */
    if (!hal_wifi_is_connected()) {
        toast("No network connection");
        return;
    }

    const char *url = get_manifest_url();
    if (!url_looks_valid(url)) {
        toast("URL not configured");
        return;
    }

    ota_check(url, on_ota_checked, NULL);
}

/* ── Build page ───────────────────────────────────────────────────────────── */

static lv_obj_t *build_update_page(struct SettingsApp *app, void *user_data) {
    (void)user_data;

    s_cards_container = NULL;

    Page page = lv_page_create("Update", true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    bool auto_up = settings_model_get_auto_update(app);

    /* Auto update switch */
    lv_obj_t *row_auto = lv_obj_create(cont);
    lv_obj_set_size(row_auto, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row_auto, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(row_auto, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row_auto, 0, 0);
    lv_obj_set_style_pad_all(row_auto, 12, 0);
    lv_obj_set_flex_flow(row_auto, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *lb_title = lv_label_create(row_auto);
    lv_label_set_text(lb_title, "Auto Update");
    lv_obj_set_style_text_font(lb_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb_title, lv_color_hex(0x666666), 0);
    lv_obj_set_style_margin_bottom(lb_title, 4, 0);

    lv_obj_t *sw_auto = lv_switch_create(row_auto);
    lv_obj_set_height(sw_auto, 24);
    if (auto_up) lv_obj_add_state(sw_auto, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_auto, on_auto_update_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* Check for update button */
    lv_obj_t *btn = lv_button_create(cont);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1976D2), 0);
    lv_obj_t *lb_btn = lv_label_create(btn);
    lv_label_set_text(lb_btn, "Check for Update");
    lv_obj_center(lb_btn);
    lv_obj_set_style_text_color(lb_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lb_btn, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(btn, on_check_update_clicked, LV_EVENT_CLICKED, NULL);

    /* ── Update cards container (populated when a new version is found) ── */
    s_cards_container = lv_obj_create(cont);
    lv_obj_set_size(s_cards_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_cards_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_cards_container, 0, 0);
    lv_obj_set_style_pad_row(s_cards_container, 8, 0);
    lv_obj_set_style_border_width(s_cards_container, 0, 0);
    lv_obj_set_style_bg_opa(s_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_cards_container, LV_SCROLLBAR_MODE_OFF);

    return page.screen;
}

void settings_view_update_init_registry(struct SettingsApp *app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_UPDATE, build_update_page);
}
