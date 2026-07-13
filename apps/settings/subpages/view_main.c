/**
 * @file view_main.c
 * @brief 设置主页 — 4 个入口项: General / WIFI / Storage / Update
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_main.h"
#include "../view.h"
#include "../app.h"
#include "lv_page.h"

extern SettingsApp g_settings_app;
extern const lv_image_dsc_t ic_info;

/* ── 设置项数据 ────────────────────────────────────────────────────────────── */

static const struct {
    const char* title;
    const void* icon;        /* string for LV_SYMBOL_*, lv_image_dsc_t* for images */
    int         page_id;
    bool        is_image;
} setting_items[] = {
    {"General",  LV_SYMBOL_SETTINGS, SETTINGS_PAGE_GENERAL, false},
    {"WIFI",     LV_SYMBOL_WIFI,     SETTINGS_PAGE_WIFI,     false},
    {"Storage",  LV_SYMBOL_SD_CARD,  SETTINGS_PAGE_STORAGE,  false},
    {"Update",   LV_SYMBOL_REFRESH,  SETTINGS_PAGE_UPDATE,   false},
    {"About",    &ic_info,           SETTINGS_PAGE_ABOUT,    true },
};

static void on_item_clicked(lv_event_t* e) {
    int page_id = (int)(uintptr_t)lv_event_get_user_data(e);
    PAGE_NAVIGATE_TO((&g_settings_app), SETTINGS_PAGE_MAIN, page_id, NULL);
}

/* ── 构建主页 (无标题栏/无返回按钮, 含 status bar) ──────────────────────── */

static lv_obj_t* build_main_page(struct SettingsApp* app, void* user_data) {
    (void)user_data;

    Page page = lv_page_create(NULL, false, NULL, NULL);
    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < 5; i++) {
        lv_obj_t* card = lv_obj_create(cont);
        lv_obj_set_size(card, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_item_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)setting_items[i].page_id);

        lv_obj_t* icon;
        if (setting_items[i].is_image) {
            icon = lv_image_create(card);
            lv_image_set_src(icon, setting_items[i].icon);
            lv_obj_set_style_img_recolor(icon, lv_color_hex(0x333333), 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            icon = lv_label_create(card);
            lv_label_set_text(icon, setting_items[i].icon);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        }
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t* title = lv_label_create(card);
        lv_label_set_text(title, setting_items[i].title);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 52, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

        lv_obj_t* arrow = lv_label_create(card);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0xCCCCCC), 0);
    }

    return page.screen;
}

void settings_view_main_init_registry(struct SettingsApp* app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_MAIN, build_main_page);
}
