#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_stations.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lang.h"

extern RadioApp g_radio_app;
extern const lv_font_t *g_cjk_font;

static void on_station_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    radio_controller_play(&g_radio_app, idx);
    radio_nav_push(&g_radio_app, PAGE_STATIONS, PAGE_PLAYING);
}

static lv_obj_t *build_stations_page(struct RadioApp *app, void *user_data) {
    (void)user_data;

    if (app->model) app->model->current_page = PAGE_STATIONS;

    Page page = lv_page_create(tr(STR_RADIO_STATIONS), false, NULL, NULL);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    int count = app->model ? app->model->count : 0;
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, tr(STR_RADIO_NO_STATIONS));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return page.screen;
    }

    for (int i = 0; i < count; i++) {
        bool active = (app->model->current == i);

        lv_obj_t *row = lv_button_create(cont);
        lv_obj_set_size(row, LV_PCT(100), 48);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, active ? lv_color_hex(0xE3F2FD) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        /* leading index number */
        char num[12];
        snprintf(num, sizeof(num), "%02d", i + 1);
        lv_obj_t *nl = lv_label_create(row);
        lv_label_set_text(nl, num);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(nl, active ? lv_color_hex(0x1E88E5) : lv_color_hex(0x999999), 0);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, 16, 0);

        /* station name — fixed-width clip box; long names scroll circularly
         * (same marquee as the podcast now-playing title) */
        const char *sname = app->model->stations[i].name;
        lv_obj_t *nm_box = lv_obj_create(row);
        lv_obj_set_size(nm_box, 185, 48);
        lv_obj_set_style_pad_all(nm_box, 0, 0);
        lv_obj_set_style_border_width(nm_box, 0, 0);
        lv_obj_set_style_radius(nm_box, 0, 0);
        lv_obj_set_style_bg_opa(nm_box, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(nm_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(nm_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* clip overflow */
        lv_obj_clear_flag(nm_box, LV_OBJ_FLAG_CLICKABLE);   /* let taps fall through to the row */
        lv_obj_align(nm_box, LV_ALIGN_LEFT_MID, 48, 0);

        lv_obj_t *nm = lv_label_create(nm_box);
        lv_label_set_text(nm, sname);
        lv_obj_set_style_text_font(nm, g_cjk_font, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_point_t tsz;
        lv_text_get_size(&tsz, sname, g_cjk_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (tsz.x > 185) {
            int32_t scroll_end = 185 - tsz.x;              /* negative: scroll left */
            uint32_t dur = (uint32_t)(-scroll_end) * 12;   /* ~12 ms/px */
            if (dur < 800) dur = 800;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, nm);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
            lv_anim_set_values(&a, 0, scroll_end);
            lv_anim_set_duration(&a, dur);
            lv_anim_set_repeat_delay(&a, 2000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }

        /* now-playing indicator */
        if (active) {
            lv_obj_t *ic = lv_label_create(row);
            lv_label_set_text(ic, LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_font(ic, g_cjk_font, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(0x1E88E5), 0);
            lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -16, 0);
        }

        /* separator */
        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, on_station_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    return page.screen;
}

void radio_view_stations_init_registry(struct RadioApp *app) {
    PAGE_REGISTE(app, PAGE_STATIONS, build_stations_page);
}
