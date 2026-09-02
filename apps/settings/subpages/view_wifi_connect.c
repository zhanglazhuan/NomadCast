/**
 * @file view_wifi_connect.c
 * @brief WiFi 连接页 — 密码输入 + 键盘, 回车触发 hal_wifi_connect
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_wifi_connect.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "wifi_cred.h"
#include "lv_page.h"
#include "hal.h"
#include "esp_log.h"

static const char *TAG = "wifi_connect";

extern SettingsApp g_settings_app;

/* ── 上下文 ────────────────────────────────────────────────────────────────── */

typedef struct {
    char ssid[32];
    lv_obj_t* pwd_ta;
    lv_obj_t* kb;
} ConnectCtx;

static ConnectCtx *g_active_connect_ctx = NULL;

static void ctx_cleanup(lv_event_t* e) {
    ConnectCtx* ctx = lv_event_get_user_data(e);
    g_active_connect_ctx = NULL;
    if (ctx) free(ctx);
}

/* ── 点击非输入区域 → 隐藏键盘 ──────────────────────────────────────────────── */

static void hide_keyboard(ConnectCtx* ctx) {
    if (!ctx || !ctx->kb) return;
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(ctx->pwd_ta, LV_STATE_FOCUSED);
}

static void show_keyboard(ConnectCtx* ctx) {
    if (!ctx || !ctx->kb) return;
    lv_obj_clear_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(ctx->pwd_ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(ctx->kb, ctx->pwd_ta);
}

static void indev_press_filter(lv_event_t* e) {
    ConnectCtx* ctx = g_active_connect_ctx;
    if (!ctx || !ctx->kb) return;

    lv_indev_t* indev = lv_event_get_target(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    bool kb_visible = !lv_obj_has_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);

    /* Check if press is within the textarea */
    lv_area_t ta_area;
    lv_obj_get_coords(ctx->pwd_ta, &ta_area);
    if (pt.x >= ta_area.x1 && pt.x <= ta_area.x2 &&
        pt.y >= ta_area.y1 && pt.y <= ta_area.y2) {
        if (!kb_visible) show_keyboard(ctx);
        return;
    }

    if (!kb_visible) return;  /* keyboard already hidden — nothing to do */

    /* Check if press is within the keyboard area */
    lv_area_t kb_area;
    lv_obj_get_coords(ctx->kb, &kb_area);
    if (pt.x >= kb_area.x1 && pt.x <= kb_area.x2 &&
        pt.y >= kb_area.y1 && pt.y <= kb_area.y2) {

        /* Bottom-left key (LV_SYMBOL_KEYBOARD) — dismiss */
        int kb_w = kb_area.x2 - kb_area.x1;
        int kb_h = kb_area.y2 - kb_area.y1;
        if (pt.x < kb_area.x1 + kb_w / 4 && pt.y > kb_area.y2 - kb_h / 4) {
            hide_keyboard(ctx);
        }
        return;
    }

    /* Press is outside both keyboard and textarea → hide keyboard */
    hide_keyboard(ctx);
}

/* ── Show password toggle ─────────────────────────────────────────────────── */

static void on_show_pwd_changed(lv_event_t* e) {
    ConnectCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* cb = lv_event_get_current_target_obj(e);
    bool show = lv_obj_has_state(cb, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(ctx->pwd_ta, !show);
}

/* ── 键盘确认 / 连接按钮 ──────────────────────────────────────────────────── */

static void do_connect(ConnectCtx* ctx) {
    const char* pwd = lv_textarea_get_text(ctx->pwd_ta);
    ESP_LOGI(TAG, "ssid='%s' pwd='%s'", ctx->ssid, pwd);

    // 调用 HAL 发起连接 (阻塞)
    hal_wifi_connect(ctx->ssid, pwd);

    // 检查结果，成功则保存密码
    bool ok = false;
    const char* err = NULL;
    hal_wifi_get_connect_result(&ok, &err);
    if (ok) {
        wifi_cred_save(ctx->ssid, pwd);
    }

    // 连接结果写入 model, 供 WIFI 页面读取
    settings_model_set_pending_connect(&g_settings_app, ctx->ssid);

    // 返回 WIFI 页面
    page_navigator_navigate_pop(&g_settings_app.view->page_nav, &g_settings_app);
}

static void on_connect_clicked(lv_event_t* e) {
    ConnectCtx* ctx = lv_event_get_user_data(e);
    do_connect(ctx);
}

static void on_ta_focused(lv_event_t* e) {
    ConnectCtx* ctx = lv_event_get_user_data(e);
    show_keyboard(ctx);
}

static void on_ta_ready(lv_event_t* e) {
    ConnectCtx* ctx = lv_event_get_user_data(e);
    if (ctx) do_connect(ctx);
}

/* ── 构建页面 ──────────────────────────────────────────────────────────────── */

static lv_obj_t* build_wifi_connect_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    ConnectCtx* ctx = malloc(sizeof(ConnectCtx));
    memset(ctx, 0, sizeof(ConnectCtx));

    if (app->view->page_nav.nav_ctx) {
        strncpy(ctx->ssid, (const char*)app->view->page_nav.nav_ctx, sizeof(ctx->ssid) - 1);
    }

    Page page = lv_page_create(ctx->ssid[0] ? ctx->ssid : "Connect",
                               true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_add_event_cb(page.screen, ctx_cleanup, LV_EVENT_DELETE, ctx);

    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_NONE);

    /* 密码输入 */
    lv_obj_t* lb = lv_label_create(cont);
    lv_label_set_text(lb, "Enter Password");
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x666666), 0);

    lv_obj_t* ta = lv_textarea_create(cont);
    lv_obj_set_size(ta, LV_PCT(100), 40);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_textarea_set_one_line(ta, true);
    ctx->pwd_ta = ta;
    lv_obj_add_event_cb(ta, on_ta_focused, LV_EVENT_FOCUSED, ctx);
    lv_obj_add_event_cb(ta, on_ta_ready, LV_EVENT_READY, ctx);

    /* Show password checkbox */
    lv_obj_t* cb = lv_checkbox_create(cont);
    lv_checkbox_set_text(cb, "Show Password");
    lv_obj_set_style_pad_all(cb, 0, 0);
    lv_obj_add_event_cb(cb, on_show_pwd_changed, LV_EVENT_VALUE_CHANGED, ctx);

    /* 连接按钮 */
    lv_obj_t* btn = lv_button_create(cont);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4CAF50), 0);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "Connect");
    lv_obj_center(bl);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(btn, on_connect_clicked, LV_EVENT_CLICKED, ctx);

    /* 键盘 (floating, 脱离 flex 布局避免溢出) */
    lv_obj_t* kb = lv_keyboard_create(page.screen);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    ctx->kb = kb;
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);  /* start hidden, show on focus */

    /* Activate indev-level touch filter — hides keyboard on taps outside
     * the textarea and keyboard area. */
    g_active_connect_ctx = ctx;
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_add_event_cb(indev, indev_press_filter, LV_EVENT_PRESSED, NULL);

    return page.screen;
}

void settings_view_wifi_connect_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_WIFI_CONNECT, build_wifi_connect_page);
}
