/**
 * @file view_wifi.c
 * @brief WIFI 设置页 — 开关 / 已连接 / 扫描列表
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_wifi.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "lv_page.h"
#include "lv_toast.h"
#include "wifi_cred.h"
#include "hal.h"

extern SettingsApp g_settings_app;

/* ── WiFi 开关 ─────────────────────────────────────────────────────────────── */

static void on_wifi_switch(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_model_set_wifi_enabled(&g_settings_app, on);
    // 刷新本页 (不推栈, 直接替换)
    page_navigator_navigate_to(&g_settings_app.view->page_nav, &g_settings_app, SETTINGS_PAGE_WIFI, NULL);
}

/* ── 扫描按钮 ──────────────────────────────────────────────────────────────── */

static void on_scan_clicked(lv_event_t* e) {
    (void)e;
    /* Non-blocking: just clear the list + mark scanning, then rebuild so the
     * page shows "Scanning..." immediately. The real scan runs in the timer. */
    settings_model_request_wifi_scan(&g_settings_app);
    page_navigator_navigate_to(&g_settings_app.view->page_nav, &g_settings_app, SETTINGS_PAGE_WIFI, NULL);
}

/* ── 异步扫描 (timer 回调) ──────────────────────────────────────────────────
 * 初始扫描 (打开 WiFi) 走 scan + auto-connect;手动 Scan 只扫描不自动连接。 */

static void async_scan_timer_cb(lv_timer_t *t)
{
    lv_obj_t *screen = (lv_obj_t *)lv_timer_get_user_data(t);
    /* Guard: screen was deleted (user navigated away) */
    if (!screen || !lv_obj_is_valid(screen)) return;

    if (g_settings_app.model->wifi_scan_autoconnect)
        settings_model_do_scan_and_connect(&g_settings_app);
    else
        settings_model_start_wifi_scan(&g_settings_app);   /* list-only */

    /* Refresh page — screen is still valid */
    page_navigator_navigate_to(&g_settings_app.view->page_nav,
                               &g_settings_app, SETTINGS_PAGE_WIFI, NULL);
}

/* ── 网络点击 ──────────────────────────────────────────────────────────────── */

static void on_network_clicked(lv_event_t* e) {
    const char* ssid = lv_event_get_user_data(e);

    /* Try auto-connect with saved password first */
    if (settings_model_auto_connect(&g_settings_app, ssid)) {
        /* Success — refresh wifi page to show connected state */
        lv_toast_show("WiFi connected", 2000);
        page_navigator_navigate_to(&g_settings_app.view->page_nav,
                                   &g_settings_app, SETTINGS_PAGE_WIFI, NULL);
        return;
    }

    /* No saved password (or saved password was wrong) — show manual connect page */
    PAGE_NAVIGATE_TO((&g_settings_app), SETTINGS_PAGE_WIFI, SETTINGS_PAGE_WIFI_CONNECT, (void*)ssid);
}

/* ── 构建 WIFI 页面 ────────────────────────────────────────────────────────── */

static lv_obj_t* build_wifi_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    Page page = lv_page_create("WIFI", true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    bool wifi_on = settings_model_get_wifi_enabled(app);

    /* ── 检查是否有待处理的连接结果 ── */
    char pending_ssid[32];
    if (settings_model_get_and_clear_pending_connect(app, pending_ssid, sizeof(pending_ssid))) {
        bool ok = false;
        const char* err = NULL;
        hal_wifi_get_connect_result(&ok, &err);
        if (ok) {
            strncpy(app->model->connected_ssid, pending_ssid, sizeof(app->model->connected_ssid) - 1);
            /* Status bar updated by APP_EVENT_WIFI_CONNECTED from HAL */
            lv_toast_show("WiFi connected", 2000);
            /* Password was already saved by view_wifi_connect's do_connect.
             * But if auto-connect was used (no view_wifi_connect involved),
             * the save already happened in settings_model_auto_connect. */
        } else {
            lv_toast_show(err ? err : "Connection failed", 2000);
        }
    }

    /* ── WiFi 开关行 (无边框平铺) ── */
    lv_obj_t* row_sw = lv_obj_create(cont);
    lv_obj_set_size(row_sw, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row_sw, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(row_sw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row_sw, 0, 0);
    lv_obj_set_style_pad_all(row_sw, 12, 0);
    lv_obj_set_flex_flow(row_sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lb_wifi = lv_label_create(row_sw);
    lv_label_set_text(lb_wifi, "WiFi");
    lv_obj_set_style_text_font(lb_wifi, &lv_font_montserrat_16, 0);

    lv_obj_t* sw_wifi = lv_switch_create(row_sw);
    lv_obj_set_height(sw_wifi, 24);
    if (wifi_on) lv_obj_add_state(sw_wifi, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_wifi, on_wifi_switch, LV_EVENT_VALUE_CHANGED, NULL);

    if (!wifi_on) return page.screen;

    /* ── 已连接 ── */
    const char* ssid = settings_model_get_connected_ssid(app);
    if (ssid) {
        lv_obj_t* row_conn = lv_obj_create(cont);
        lv_obj_set_size(row_conn, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row_conn, lv_color_hex(0xE8F5E9), 0);
        lv_obj_set_style_bg_opa(row_conn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row_conn, 0, 0);
        lv_obj_set_style_pad_all(row_conn, 12, 0);
        lv_obj_set_flex_flow(row_conn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_conn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row_conn, 8, 0);

        lv_obj_t* icon = lv_label_create(row_conn);
        lv_label_set_text(icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x4CAF50), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);

        lv_obj_t* lb_conn = lv_label_create(row_conn);
        lv_label_set_text(lb_conn, ssid);
        lv_obj_set_style_text_font(lb_conn, &lv_font_montserrat_14, 0);
    }

    /* ── 扫描按钮 ── */
    lv_obj_t* btn_scan = lv_button_create(cont);
    lv_obj_set_size(btn_scan, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x1976D2), 0);
    lv_obj_t* lb_scan = lv_label_create(btn_scan);
    lv_label_set_text(lb_scan, "Scan for Networks");
    lv_obj_center(lb_scan);
    lv_obj_set_style_text_color(lb_scan, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(btn_scan, on_scan_clicked, LV_EVENT_CLICKED, NULL);

    /* ── 扫描中: 清空列表, 显示 Scanning... 并启动实际扫描 ── */
    if (app->model->wifi_scanning) {
        lv_obj_add_state(btn_scan, LV_STATE_DISABLED);   /* 防止扫描中重复点击 */

        lv_obj_t* lb_scanning = lv_label_create(cont);
        lv_label_set_text(lb_scanning, "Scanning...");
        lv_obj_set_style_text_font(lb_scanning, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lb_scanning, lv_color_hex(0x666666), 0);
        lv_obj_set_style_pad_top(lb_scanning, 8, 0);

        /* One-shot timer: run the (blocking) scan, then refresh.
         * Auto-cancels if the screen is deleted (user navigates away). */
        lv_timer_t* t = lv_timer_create(async_scan_timer_cb, 200, page.screen);
        lv_timer_set_repeat_count(t, 1);

        return page.screen;   /* 扫描中不渲染旧列表 */
    }

    /* ── 扫描结果 ── */
    int count = settings_model_get_scanned_count(app);
    const WifiNetwork* nets = settings_model_get_scanned_networks(app);
    if (count > 0) {
        lv_obj_t* lb_title = lv_label_create(cont);
        lv_label_set_text_fmt(lb_title, "Available Networks (%d)", count);
        lv_obj_set_style_text_font(lb_title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lb_title, lv_color_hex(0x666666), 0);

        for (int i = 0; i < count; i++) {
            lv_obj_t* row = lv_obj_create(cont);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 12, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

            // 点击进入连接页，通过 user_data 传 SSID
            lv_obj_add_event_cb(row, on_network_clicked, LV_EVENT_CLICKED, (void*)nets[i].ssid);

            lv_obj_t* sig = lv_label_create(row);
            lv_label_set_text(sig, LV_SYMBOL_WIFI);
            if (nets[i].signal_strength >= 60)
                lv_obj_set_style_text_color(sig, lv_color_hex(0x4CAF50), 0);
            else if (nets[i].signal_strength >= 30)
                lv_obj_set_style_text_color(sig, lv_color_hex(0x8BC34A), 0);
            else
                lv_obj_set_style_text_color(sig, lv_color_hex(0x9E9E9E), 0);

            lv_obj_t* name = lv_label_create(row);
            lv_label_set_text(name, nets[i].ssid);
            lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
            lv_obj_set_flex_grow(name, 1);

            if (nets[i].secured) {
                lv_obj_t* lock = lv_label_create(row);
                lv_label_set_text(lock, "*");
                lv_obj_set_style_text_color(lock, lv_color_hex(0x999999), 0);
            }
        }
    }

    return page.screen;
}

void settings_view_wifi_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_WIFI, build_wifi_page);
}
