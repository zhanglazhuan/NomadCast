/**
 * @file view_settings.c
 * @brief 设置页 — 国家 (下拉) + 下载音质 (下拉),平铺标签无卡片
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_settings.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "lv_page.h"
#include "flash_store.h"
#include "lang.h"
#include "countries.h"
#include "app_event.h"

extern PodcastApp g_podcast_app;

// ---- 设置项标签 (平铺, 无卡片, 减小上下间距) ----
static void add_setting_label(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, g_cjk_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x666666), 0);
    lv_obj_set_style_margin_top(l, 10, 0);   /* separates each group; dropdown stays tight below */
}

/* ── Country (Apple Podcasts region) ─────────────────────────────────────── */
/* Storefront code ↔ localized name lives in the shared system/lang/countries
 * table; the code is persisted in flash ("podcast"/"country"). */

static void on_country_changed(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    flash_set_str("podcast", "country", country_code_at(sel));
    app_event_fire(APP_EVENT_COUNTRY_CHANGED, NULL);
}

static void on_quality_changed(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    podcast_model_set_download_quality(&g_podcast_app, sel);
}

static lv_obj_t* build_settings_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;
    Page page = lv_page_create(tr(STR_APP_SETTINGS), true, page_navigator_navigate_back, &app->view->page_nav);

    lv_obj_t* cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_pad_hor(cont, 12, 0);
    lv_obj_set_style_pad_ver(cont, 8, 0);
    lv_obj_set_style_pad_row(cont, 2, 0);   /* tight: label sits just above its dropdown */

    int quality = podcast_model_get_download_quality(app);

    // ---- 国家/地区 (Apple Podcasts region) ----
    char cc[8];
    flash_get_str("podcast", "country", cc, sizeof(cc), "cn");
    int country_idx = country_index_of(cc);
    if (country_idx < 0) country_idx = 0;

    char country_opts[512];
    country_options_str(country_opts, sizeof(country_opts));

    add_setting_label(cont, tr(STR_COUNTRY));
    lv_obj_t* ccd = lv_dropdown_create(cont);
    lv_obj_set_width(ccd, LV_PCT(100));
    lv_obj_set_style_text_font(ccd, g_cjk_font, 0);
    lv_dropdown_set_options(ccd, country_opts);
    lv_dropdown_set_selected(ccd, country_idx);
    lv_obj_add_event_cb(ccd, on_country_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t* ccd_list = lv_dropdown_get_list(ccd);
    lv_obj_set_style_text_font(ccd_list, g_cjk_font, 0);

    // ---- 下载音质 (下拉) ----
    add_setting_label(cont, tr(STR_DOWNLOAD_QUALITY));
    lv_obj_t* qd = lv_dropdown_create(cont);
    lv_obj_set_width(qd, LV_PCT(100));
    lv_obj_set_style_text_font(qd, g_cjk_font, 0);
    lv_dropdown_set_options(qd, tr(STR_QUALITY_OPTIONS));
    lv_dropdown_set_selected(qd, quality);
    lv_obj_add_event_cb(qd, on_quality_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t* qd_list = lv_dropdown_get_list(qd);
    lv_obj_set_style_text_font(qd_list, g_cjk_font, 0);

    printf("[INF] Settings page built\n"); fflush(stdout);
    return page.screen;
}

void podcast_view_settings_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_SETTINGS, build_settings_page);
}
