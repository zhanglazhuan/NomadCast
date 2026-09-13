#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "view_alarm_list.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "alarm_service.h"
#include "lv_page.h"
#include "lv_home_indicator.h"
#include "lang.h"

extern AlarmApp g_alarm_app;
extern const lv_font_t *g_cjk_font;

static lv_point_t s_press_pt = { 0, 0 };   /* touch-down point, for swipe-vs-tap */

static void on_row_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &s_press_pt);
}

static void on_row_clicked(lv_event_t *e)
{
    /* A swipe must not fire the tap handler. */
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        if (lv_indev_get_gesture_dir(indev) != LV_DIR_NONE ||
            lv_indev_get_press_moved(indev))
            return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (LV_ABS(pt.x - s_press_pt.x) + LV_ABS(pt.y - s_press_pt.y) > 20)
            return;
    }

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_alarm_app.model->editing_index = idx;
    PAGE_NAVIGATE_TO(&g_alarm_app, PAGE_LIST, PAGE_EDIT, (void*)(intptr_t)idx);
}

static void on_add_clicked(lv_event_t *e)
{
    (void)e;
    g_alarm_app.model->editing_index = -1;
    PAGE_NAVIGATE_TO(&g_alarm_app, PAGE_LIST, PAGE_EDIT, (void*)(intptr_t)-1);
}

static void on_switch_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const alarm_entry_t *a = alarm_service_get(idx);
    if (!a) return;
    bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    alarm_service_set(idx, a->hour, a->minute, en, a->repeat, a->weekdays);
}

static lv_obj_t *build_alarm_list_page(struct AlarmApp *app, void *user_data)
{
    (void)user_data;

    if (app->model) app->model->current_page = PAGE_LIST;

    Page page = lv_page_create(tr(STR_APP_ALARM), false, NULL, NULL);
    lv_obj_t *cont = page.container;
    lv_home_indicator_create(page.screen);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    /* "+" button in the header's right slot */
    if (page.header_right) {
        lv_obj_set_width(page.header_right, 40);
        lv_obj_t *add_btn = lv_button_create(page.header_right);
        lv_obj_set_size(add_btn, 24, 24);
        lv_obj_set_style_bg_opa(add_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(add_btn, 0, 0);
        lv_obj_set_style_shadow_width(add_btn, 0, 0);
        lv_obj_set_style_pad_all(add_btn, 0, 0);
        lv_obj_align(add_btn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(add_btn, on_add_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_t *plus = lv_label_create(add_btn);
        lv_label_set_text(plus, "+");
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(plus, lv_color_hex(0x1976D2), 0);
        lv_obj_center(plus);
    }

    int count = alarm_service_count();
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, tr(STR_ALARM_EMPTY));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return page.screen;
    }

    for (int i = 0; i < count; i++) {
        const alarm_entry_t *a = alarm_service_get(i);
        if (!a) continue;

        lv_obj_t *row = lv_button_create(cont);
        lv_obj_set_size(row, LV_PCT(100), 64);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        /* time */
        char time_str[8];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", a->hour, a->minute);
        lv_obj_t *tl = lv_label_create(row);
        lv_label_set_text(tl, time_str);
        lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(tl, a->enabled ? lv_color_hex(0x1976D2)
                                                   : lv_color_hex(0xBBBBBB), 0);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 20, -10);

        /* repeat summary (secondary line) */
        char summary[64];
        alarm_repeat_summary(a, summary, sizeof(summary));
        lv_obj_t *sl = lv_label_create(row);
        lv_label_set_text(sl, summary);
        lv_obj_set_style_text_font(sl, g_cjk_font ? g_cjk_font : &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(0x999999), 0);
        lv_obj_align(sl, LV_ALIGN_LEFT_MID, 20, 14);

        /* enable switch */
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 44, 24);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
        if (a->enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_switch_changed, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);

        /* separator */
        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, on_row_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    }

    return page.screen;
}

void alarm_view_list_init_registry(struct AlarmApp *app) {
    PAGE_REGISTE(app, PAGE_LIST, build_alarm_list_page);
}
