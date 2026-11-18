#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_favorites.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lang.h"

extern RadioApp g_radio_app;
extern const lv_font_t *g_cjk_font;

static void on_fav_click(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    radio_controller_play(&g_radio_app, RADIO_SRC_FAVORITES, idx);
    radio_nav_push(&g_radio_app, PAGE_FAVORITES, PAGE_PLAYING);
}

static lv_obj_t *build_favorites_page(struct RadioApp *app, void *user_data) {
    (void)user_data;
    RadioModel *m = app->model;
    if (m) m->current_page = PAGE_FAVORITES;

    Page page = lv_page_create(tr(STR_RADIO_TAB_FAVORITES), false, NULL, NULL);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    int count = m ? m->favorite_count : 0;
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(cont);
        lv_label_set_text(lbl, tr(STR_RADIO_NO_FAVORITES));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        radio_view_create_bottom_tab_bar(page.screen, RADIO_TAB_FAVORITES);
        return page.screen;
    }

    for (int i = 0; i < count; i++) {
        const radio_station_t *s = &m->favorites[i];

        lv_obj_t *row = lv_button_create(cont);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        /* name */
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s->name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(nm, g_cjk_font, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0x333333), 0);
        lv_obj_set_width(nm, 165);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 16, -8);

        /* region + codec/bitrate subtitle */
        char sub[96];
        if (s->bitrate > 0 && s->codec[0])
            snprintf(sub, sizeof(sub), "%s %s %dkbps", s->region_name, s->codec, s->bitrate);
        else
            snprintf(sub, sizeof(sub), "%s", s->region_name);
        lv_obj_t *sl = lv_label_create(row);
        lv_label_set_text(sl, sub);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sl, g_cjk_font, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(0x999999), 0);
        lv_obj_set_width(sl, 165);
        lv_obj_align(sl, LV_ALIGN_LEFT_MID, 16, 10);

        /* heart glyph */
        lv_obj_t *ht = lv_label_create(row);
        lv_label_set_text(ht, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(ht, g_cjk_font, 0);
        lv_obj_set_style_text_color(ht, lv_color_hex(0xE53935), 0);
        lv_obj_align(ht, LV_ALIGN_RIGHT_MID, -16, 0);

        /* separator */
        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, on_fav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    radio_view_create_bottom_tab_bar(page.screen, RADIO_TAB_FAVORITES);
    return page.screen;
}

void radio_view_favorites_init_registry(struct RadioApp *app) {
    PAGE_REGISTE(app, PAGE_FAVORITES, build_favorites_page);
}
