/**
 * @file view_ota_status.c
 * @brief OTA 状态页 — 展示固件升级进度条
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_ota_status.h"
#include "../view.h"
#include "../app.h"
#include "lv_page.h"

/* OTA 进度共享变量:-1 空闲, 0..100 进度, -2 失败。
 * 由 esp_event 任务写 (view_update.c 的 on_ota_progress),本页 LVGL 定时器读。 */
volatile int g_ota_progress = -1;

static lv_obj_t   *s_bar = NULL;
static lv_obj_t   *s_label = NULL;
static lv_timer_t *s_timer = NULL;

static void on_page_delete(lv_event_t *e) {
    (void)e;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_bar = NULL;
    s_label = NULL;
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    int pct = g_ota_progress;
    if (pct == -2) {                       /* OTA worker 报告失败 */
        if (s_label) lv_label_set_text(s_label, "Update failed");
        lv_timer_del(t);
        s_timer = NULL;
        return;
    }
    if (pct >= 0 && s_bar && s_label) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_label, "%d%%", pct);
    }
}

static lv_obj_t *build_ota_status_page(struct SettingsApp *app, void *user_data) {
    (void)user_data;

    Page page = lv_page_create("OTA Status", true, page_navigator_navigate_back,
                               &app->view->page_nav);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 24, 0);
    lv_obj_set_style_pad_row(cont, 16, 0);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_recolor(title, true);
    lv_label_set_text(title, "Upgrading. #FF0000 Do not power off.#");
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(90));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    s_bar = lv_bar_create(cont);
    lv_obj_set_size(s_bar, LV_PCT(80), 12);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_label = lv_label_create(cont);
    lv_label_set_text(s_label, "0%");
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_16, 0);

    lv_obj_add_event_cb(page.screen, on_page_delete, LV_EVENT_DELETE, NULL);
    s_timer = lv_timer_create(poll_cb, 200, NULL);

    return page.screen;
}

void settings_view_ota_status_init_registry(struct SettingsApp *app) {
    PAGE_REGISTE(app, SETTINGS_PAGE_OTA_STATUS, build_ota_status_page);
}
