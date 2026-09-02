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

extern SettingsApp g_settings_app;

/* ── 清理按钮回调 ──────────────────────────────────────────────────────────── */

static void on_clean_clicked(lv_event_t* e) {
    (void)e;
    settings_model_clean_storage(&g_settings_app);
    page_navigator_navigate_to(&g_settings_app.view->page_nav, &g_settings_app, SETTINGS_PAGE_STORAGE, NULL);
}

/* ── 构建 Storage 页面 ──────────────────────────────────────────────────────── */

static lv_obj_t* build_storage_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    Page page = lv_page_create("Storage", true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    int used  = settings_model_get_storage_used_mb(app);
    int total = settings_model_get_storage_total_mb(app);

    /* ── No SD card ── */
    if (used < 0 || total <= 0) {
        lv_obj_t* lb = lv_label_create(cont);
        lv_label_set_text(lb, "No SD Card Insert ~");
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x999999), 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        return page.screen;
    }

    /* ── 标题 ── */
    lv_obj_t* lb_title = lv_label_create(cont);
    lv_label_set_text(lb_title, "Storage Usage");
    lv_obj_set_style_text_font(lb_title, &lv_font_montserrat_16, 0);
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
    lv_label_set_text(lb_btn, "Clean Storage");
    lv_obj_center(lb_btn);
    lv_obj_set_style_text_color(lb_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lb_btn, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(btn, on_clean_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

    return page.screen;
}

void settings_view_storage_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_STORAGE, build_storage_page);
}
