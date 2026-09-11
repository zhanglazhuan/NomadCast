#include <stdint.h>
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_home_indicator.h"
#include "lang.h"

extern PlayerApp g_player_app;
extern const lv_font_t *g_cjk_font;

static const int TAB_STR_KEYS[PLAYER_TAB_COUNT] = {
    STR_TAB_FILES,
    STR_TAB_PLAY,
};

static const int TAB_PAGE_IDS[PLAYER_TAB_COUNT] = {
    PAGE_FILES,
    PAGE_PLAYER,
};

static void on_tab_clicked(lv_event_t* e) {
    player_main_tab_t tab = (player_main_tab_t)(uintptr_t)lv_event_get_user_data(e);
    int from = g_player_app.model ? g_player_app.model->current_page : PAGE_NONE;
    player_nav_push(&g_player_app, from, TAB_PAGE_IDS[tab]);
}

lv_obj_t* player_view_create_bottom_tab_bar(lv_obj_t* parent, player_main_tab_t active) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);   /* tab bar must not scroll */
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_size(bar, LV_PCT(100), PLAYER_TAB_BAR_HEIGHT);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < PLAYER_TAB_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(bar);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, PLAYER_TAB_BAR_HEIGHT);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, tr(TAB_STR_KEYS[i]));
        lv_obj_set_style_text_font(label, g_cjk_font, 0);
        lv_obj_center(label);
        if (i == (int)active) {
            lv_obj_set_style_text_color(label, lv_color_hex(0x1976D2), 0);
        } else {
            lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
        }

        lv_obj_add_event_cb(btn, on_tab_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }

    /* Home indicator floats over the tab bar's empty bottom margin. */
    lv_home_indicator_create(parent);

    return bar;
}
