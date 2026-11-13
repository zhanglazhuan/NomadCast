/**
 * @file view_storage.c
 * @brief Storage 设置页 — 用量进度条 + 清理按钮 (暂不开放)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_storage.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "lv_page.h"
#include "lang.h"
#include "usb_storage.h"

extern SettingsApp g_settings_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    lv_obj_t *section;
    lv_obj_t *hint;
    lv_obj_t *sw;
    lv_timer_t *timer;
} storage_usb_ctx_t;

static void refresh_usb(lv_timer_t *timer)
{
    storage_usb_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (usb_storage_connected()) lv_obj_remove_flag(ctx->section, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ctx->section, LV_OBJ_FLAG_HIDDEN);
    if (usb_storage_enabled()) lv_obj_add_state(ctx->sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(ctx->sw, LV_STATE_CHECKED);
    if (usb_storage_busy() || !usb_storage_has_card()) lv_obj_add_state(ctx->sw, LV_STATE_DISABLED);
    else lv_obj_remove_state(ctx->sw, LV_STATE_DISABLED);
    lv_label_set_text(ctx->hint, tr(usb_storage_busy() ? STR_USB_SWITCHING :
                                   usb_storage_last_error() != ESP_OK ? STR_USB_FAILED : STR_USB_HINT));
}

static void delete_usb_ctx(lv_event_t *e)
{
    storage_usb_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->timer) lv_timer_delete(ctx->timer);
    lv_free(ctx);
}

static void toggle_usb(lv_event_t *e)
{
    storage_usb_ctx_t *ctx = lv_event_get_user_data(e);
    bool enabled = lv_obj_has_state(ctx->sw, LV_STATE_CHECKED);
    usb_storage_set_enabled(enabled);
    refresh_usb(ctx->timer);
}

static void build_usb_controls(lv_obj_t *screen, lv_obj_t *cont)
{
    storage_usb_ctx_t *ctx = lv_malloc_zeroed(sizeof(*ctx));
    if (!ctx) return;
    ctx->section = lv_obj_create(cont);
    lv_obj_remove_style_all(ctx->section);
    lv_obj_set_size(ctx->section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ctx->section, 8, 0);
    lv_obj_t *row = lv_obj_create(ctx->section);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, tr(STR_USB_DISK));
    lv_obj_set_style_text_font(title, g_cjk_font, 0);
    ctx->sw = lv_switch_create(row);
    lv_obj_add_event_cb(ctx->sw, toggle_usb, LV_EVENT_VALUE_CHANGED, ctx);
    /* Reminder lives directly below the switch row. No connection-status label. */
    ctx->hint = lv_label_create(ctx->section);
    lv_obj_set_width(ctx->hint, LV_PCT(100));
    lv_obj_set_style_text_font(ctx->hint, g_cjk_font, 0);
    lv_obj_set_style_text_color(ctx->hint, lv_color_hex(0x888888), 0);
    ctx->timer = lv_timer_create(refresh_usb, 250, ctx);
    lv_obj_add_event_cb(screen, delete_usb_ctx, LV_EVENT_DELETE, ctx);
    refresh_usb(ctx->timer);
}

/* ── 清理按钮回调 ──────────────────────────────────────────────────────────── */

static void on_clean_clicked(lv_event_t* e) {
    (void)e;
    settings_model_clean_storage(&g_settings_app);
    page_navigator_navigate_to(&g_settings_app.view->page_nav, &g_settings_app, SETTINGS_PAGE_STORAGE, NULL);
}

/* ── 构建 Storage 页面 ──────────────────────────────────────────────────────── */

static lv_obj_t* build_storage_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    Page page = lv_page_create(tr(STR_STORAGE), true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    int used  = settings_model_get_storage_used_mb(app);
    int total = settings_model_get_storage_total_mb(app);

    /* ── No SD card ── */
    if (used < 0 || total <= 0) {
        if (!usb_storage_has_card()) {
            lv_obj_t* lb = lv_label_create(cont);
            lv_label_set_text(lb, tr(STR_NO_SD_CARD));
            lv_obj_set_style_text_font(lb, g_cjk_font, 0);
            lv_obj_set_style_text_color(lb, lv_color_hex(0x999999), 0);
        }
        build_usb_controls(page.screen, cont);
        return page.screen;
    }

    /* ── 标题 ── */
    lv_obj_t* lb_title = lv_label_create(cont);
    lv_label_set_text(lb_title, tr(STR_STORAGE_USAGE));
    lv_obj_set_style_text_font(lb_title, g_cjk_font, 0);
    lv_obj_set_style_text_color(lb_title, lv_color_hex(0x333333), 0);

    /* ── 进度条 ── */
    lv_obj_t* bar = lv_bar_create(cont);
    lv_obj_set_size(bar, LV_PCT(100), 16);
    lv_bar_set_range(bar, 0, total);
    lv_bar_set_value(bar, used, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);

    lv_color_t bar_color = lv_color_hex(0x4CAF50);
    if (used > total * 3 / 4) bar_color = lv_color_hex(0xF44336);
    else if (used > total / 2) bar_color = lv_color_hex(0xFF9800);
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);

    /* ── 数值 (整数 MB — LVGL printf 不支持 %f) ── */
    lv_obj_t* lb_info = lv_label_create(cont);
    lv_label_set_text_fmt(lb_info, "%d MB / %d MB  (%d%%)",
                          used, total, total > 0 ? used * 100 / total : 0);
    lv_obj_set_style_text_font(lb_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb_info, lv_color_hex(0x888888), 0);

    /* ── 清理按钮 (暂不开放) ── */
    lv_obj_t* btn = lv_button_create(cont);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF44336), 0);
    lv_obj_t* lb_btn = lv_label_create(btn);
    lv_label_set_text(lb_btn, tr(STR_CLEAN_STORAGE));
    lv_obj_center(lb_btn);
    lv_obj_set_style_text_color(lb_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lb_btn, g_cjk_font, 0);
    lv_obj_add_event_cb(btn, on_clean_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

    build_usb_controls(page.screen, cont);
    return page.screen;
}

void settings_view_storage_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_STORAGE, build_storage_page);
}
