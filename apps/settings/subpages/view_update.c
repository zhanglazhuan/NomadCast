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
#include "ota.h"
#include "flash_store.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern SettingsApp g_settings_app;

/* Default manifest URL — empty means "not configured".
 * Flash key "upd_url" can override it. */
#define DEFAULT_MANIFEST_URL "http://192.168.137.1:5000/api/ota/check"

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static const char *get_manifest_url(void) {
    static char url[256];
    flash_get_str("settings", "upd_url", url, sizeof(url), "");
    if (url[0] == '\0') {
        /* No URL configured — use default */
        return DEFAULT_MANIFEST_URL;
    }
    return url;
}

static bool url_looks_valid(const char *url) {
    if (!url || url[0] == '\0') return false;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

/* ── Toast wrappers ─────────────────────────────────────────────────────── */

static void toast(const char *msg) { lv_toast_show(msg, 3000); }

/* ── Dialog helpers ─────────────────────────────────────────────────────── */

static void delete_parent_parent(lv_event_t *e) {
    lv_obj_t *o = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    lv_obj_delete(o);
}

/* ── Auto update switch ───────────────────────────────────────────────────── */

static void on_auto_update_switch(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_model_set_auto_update(&g_settings_app, on);
}

/* ── OTA check callback ───────────────────────────────────────────────────── */

/* ── Firmware URL preserved across OTA check → install flow ─────────────── */
static char s_firmware_url[512];

static void on_ota_progress(int percent, void *user_data)
{
    (void)user_data;
    /* Simple toast feedback */
    static char buf[32];
    snprintf(buf, sizeof(buf), "Updating... %d%%", percent);
    lv_toast_show(buf, 1000);
}

static void on_install_clicked(lv_event_t *e)
{
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
    lv_obj_delete(overlay);

    lv_toast_show("Downloading update...", 2000);
    if (ota_perform(s_firmware_url, on_ota_progress, NULL)) {
        lv_toast_show("Update complete — rebooting", 2000);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        lv_toast_show("Update failed", 3000);
    }
}

static void on_ota_checked(ota_check_result_t result,
                            const char *new_version,
                            const char *changelog,
                            const char *firmware_url,
                            void *user_data)
{
    (void)user_data;

    switch (result) {
    case OTA_CHECK_UP_TO_DATE:
        toast("Already up-to-date");
        break;

    case OTA_CHECK_UPDATE_AVAILABLE: {
        if (firmware_url) snprintf(s_firmware_url, sizeof(s_firmware_url), "%s", firmware_url);
        lv_obj_t *scr = lv_screen_active();
        lv_obj_t *overlay = lv_obj_create(scr);
        lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_center(overlay);

        lv_obj_t *dlg = lv_obj_create(overlay);
        lv_obj_set_size(dlg, 220, 160);
        lv_obj_center(dlg);
        lv_obj_set_style_radius(dlg, 8, 0);
        lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(dlg, 16, 0);
        lv_obj_set_style_pad_row(dlg, 8, 0);

        lv_obj_t *title = lv_label_create(dlg);
        lv_label_set_text_fmt(title, "New version: %s", new_version);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

        if (changelog && changelog[0]) {
            lv_obj_t *cl = lv_label_create(dlg);
            lv_label_set_text(cl, changelog);
            lv_obj_set_style_text_color(cl, lv_color_hex(0x666666), 0);
            lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
        }

        lv_obj_t *btn_row = lv_obj_create(dlg);
        lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(btn_row, 0, 0);
        lv_obj_set_style_border_width(btn_row, 0, 0);
        lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);

        lv_obj_t *cancel = lv_button_create(btn_row);
        lv_obj_set_flex_grow(cancel, 1);
        lv_obj_t *ca_lb = lv_label_create(cancel);
        lv_label_set_text(ca_lb, "Later");
        lv_obj_center(ca_lb);
        lv_obj_add_event_cb(cancel, delete_parent_parent, LV_EVENT_CLICKED, NULL);

        lv_obj_t *install = lv_button_create(btn_row);
        lv_obj_set_flex_grow(install, 1);
        lv_obj_set_style_bg_color(install, lv_color_hex(0x4CAF50), 0);
        lv_obj_t *in_lb = lv_label_create(install);
        lv_label_set_text(in_lb, "Install");
        lv_obj_set_style_text_color(in_lb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(in_lb);
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

    toast("Checking for updates...");
    ota_check(url, on_ota_checked, NULL);
}

/* ── Build page ───────────────────────────────────────────────────────────── */

static lv_obj_t *build_update_page(struct SettingsApp *app, void *user_data) {
    (void)user_data;

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

    /* Description */
    lv_obj_t *lb_desc = lv_label_create(cont);
    lv_label_set_text(lb_desc, auto_up
        ? "Updates will be downloaded and installed automatically."
        : "You will be notified when updates are available.");
    lv_obj_set_style_text_font(lb_desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lb_desc, lv_color_hex(0x888888), 0);

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

    return page.screen;
}

void settings_view_update_init_registry(struct SettingsApp *app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_UPDATE, build_update_page);
}
