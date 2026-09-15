#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_city.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lang.h"
#include "pinyin_zh_cn.h"

extern WeatherApp g_weather_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    lv_obj_t *textarea;
    lv_obj_t *kb;
    lv_obj_t *ime;
    lv_obj_t *cand;
} CityPageCtx;

static CityPageCtx *g_active_city_ctx = NULL;

/* ── 键盘显隐:点搜索栏弹出、点非键盘区域收起 ─────────────────────────────── */

static bool pt_in_area(const lv_point_t *pt, const lv_area_t *a) {
    return pt->x >= a->x1 && pt->x <= a->x2 && pt->y >= a->y1 && pt->y <= a->y2;
}

static void show_keyboard(CityPageCtx *ctx) {
    if (!ctx || !ctx->kb || !ctx->textarea) return;
    lv_obj_clear_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(ctx->textarea, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(ctx->kb, ctx->textarea);
}

static void hide_keyboard(CityPageCtx *ctx) {
    if (!ctx || !ctx->kb) return;
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    if (ctx->textarea) lv_obj_clear_state(ctx->textarea, LV_STATE_FOCUSED);
    if (ctx->cand) lv_obj_add_flag(ctx->cand, LV_OBJ_FLAG_HIDDEN);
}

static void indev_press_filter(lv_event_t *e) {
    CityPageCtx *ctx = g_active_city_ctx;
    if (!ctx || !ctx->kb || !ctx->textarea) return;

    lv_indev_t *indev = lv_event_get_target(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    bool kb_visible = !lv_obj_has_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);

    lv_area_t ta_area;
    lv_obj_get_coords(ctx->textarea, &ta_area);
    if (pt_in_area(&pt, &ta_area)) {
        if (!kb_visible) show_keyboard(ctx);
        return;
    }

    if (!kb_visible) return;

    lv_area_t kb_area;
    lv_obj_get_coords(ctx->kb, &kb_area);
    if (pt_in_area(&pt, &kb_area)) return;

    if (ctx->cand && !lv_obj_has_flag(ctx->cand, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t cand_area;
        lv_obj_get_coords(ctx->cand, &cand_area);
        if (pt_in_area(&pt, &cand_area)) return;
    }

    hide_keyboard(ctx);
}

/* ── 键盘中英切换 (左下角 中/EN 键) — copied from podcast view_search.c ───── */

#define KB_LANG_BTN "中/EN"
#define KB_POP(w)   (LV_BUTTONMATRIX_CTRL_POPOVER | (w))

static bool s_kb_en_mode = false;

static const char *const s_kb_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    KB_LANG_BTN, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t s_kb_ctrl_lc[] = {
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5, KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), KB_POP(4), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6, KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), KB_POP(3), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_BUTTONMATRIX_CTRL_CHECKED | KB_POP(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_POP(1), KB_POP(1), KB_POP(1), KB_POP(1), KB_POP(1), KB_POP(1), KB_POP(1), KB_POP(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_POP(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_POP(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_POP(1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 4, LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};

static void on_kb_lang(lv_event_t *e) {
    lv_obj_t *kb  = lv_event_get_current_target(e);
    lv_obj_t *ime = lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = (id == LV_BUTTONMATRIX_BUTTON_NONE)
                      ? NULL : lv_buttonmatrix_get_button_text(kb, id);

    if (txt && strcmp(txt, KB_LANG_BTN) == 0) {
        s_kb_en_mode = !s_kb_en_mode;
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
        if (cand && s_kb_en_mode) lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_keyboard_def_event_cb(e);

    if (s_kb_en_mode) {
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
        if (cand) lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 搜索 / 选择 / 自动定位 ───────────────────────────────────────────────── */

static void do_search(const char *query) {
    if (!query || !query[0]) return;
    WeatherModel *m = g_weather_app.model;
    if (m) strncpy(m->search_query, query, sizeof(m->search_query) - 1);
    weather_controller_search(&g_weather_app, query);
    hide_keyboard(g_active_city_ctx);
}

static void on_enter(lv_event_t *e) {
    CityPageCtx *ctx = lv_event_get_user_data(e);
    do_search(lv_textarea_get_text(ctx->textarea));
}

static void on_result_click(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    weather_controller_apply_city(&g_weather_app, idx);
    page_navigator_navigate_pop(&g_weather_app.view->page_nav, &g_weather_app);
}

static void on_auto_switch(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    WeatherModel *m = g_weather_app.model;
    if (!m) return;
    m->use_ip = on;
    weather_model_save_location(&g_weather_app);
    if (on) weather_controller_locate(&g_weather_app);
}

static void ctx_cleanup_cb(lv_event_t *e) {
    CityPageCtx *ctx = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_remove_event_cb_with_user_data(indev, indev_press_filter, NULL);
    g_active_city_ctx = NULL;
    if (ctx) free(ctx);
}

static lv_obj_t *build_city_page(struct WeatherApp *app, void *user_data) {
    (void)user_data;
    WeatherModel *m = app->model;
    if (m) m->current_page = PAGE_CITY;

    Page page = lv_page_create(tr(STR_WEATHER_MANUAL), true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t *cont = page.container;

    CityPageCtx *ctx = (CityPageCtx *)calloc(1, sizeof(CityPageCtx));
    lv_obj_add_event_cb(page.screen, ctx_cleanup_cb, LV_EVENT_DELETE, ctx);

    /* ── auto-locate toggle ── */
    lv_obj_t *togglerow = lv_obj_create(cont);
    lv_obj_set_size(togglerow, LV_PCT(100), 40);
    lv_obj_set_style_pad_hor(togglerow, 8, 0);
    lv_obj_set_style_border_width(togglerow, 0, 0);
    lv_obj_set_flex_flow(togglerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(togglerow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_lbl = lv_label_create(togglerow);
    lv_label_set_text(auto_lbl, tr(STR_WEATHER_AUTO_LOCATE));
    lv_obj_set_style_text_font(auto_lbl, g_cjk_font, 0);

    lv_obj_t *sw = lv_switch_create(togglerow);
    if (m && m->use_ip) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_auto_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── search bar (自动定位开关正下方,键盘/候选栏在其下方,不遮挡) ── */
    lv_obj_t *search_bar = lv_obj_create(cont);
    lv_obj_set_size(search_bar, LV_PCT(100), 44);
    lv_obj_set_style_border_width(search_bar, 0, 0);
    lv_obj_set_style_pad_all(search_bar, 6, 0);

    lv_obj_t *ta = lv_textarea_create(search_bar);
    lv_obj_set_size(ta, LV_PCT(100), 32);
    lv_textarea_set_placeholder_text(ta, tr(STR_WEATHER_SEARCH_HINT));
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 4, 0);
    lv_textarea_set_one_line(ta, true);
    if (m && m->search_query[0]) lv_textarea_set_text(ta, m->search_query);
    ctx->textarea = ta;
    lv_obj_add_event_cb(ta, on_enter, LV_EVENT_READY, ctx);

    /* ── results ── */
    lv_obj_t *results = lv_obj_create(cont);
    lv_obj_set_size(results, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(results, 1);
    lv_obj_set_flex_flow(results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(results, 0, 0);
    lv_obj_set_style_pad_all(results, 4, 0);
    lv_obj_set_style_pad_row(results, 2, 0);
    lv_obj_set_scroll_dir(results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(results, LV_SCROLLBAR_MODE_AUTO);

    if (m && m->search_done) {
        if (m->search_count == 0) {
            lv_obj_t *empty = lv_label_create(results);
            lv_label_set_text(empty, tr(STR_WEATHER_NO_RESULTS));
            lv_obj_set_style_text_font(empty, g_cjk_font, 0);
            lv_obj_set_style_text_color(empty, lv_color_hex(0x999999), 0);
            lv_obj_center(empty);
        } else {
            for (int i = 0; i < m->search_count; i++) {
                lv_obj_t *row = lv_button_create(results);
                lv_obj_set_size(row, LV_PCT(100), 44);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_radius(row, 6, 0);
                lv_obj_set_style_pad_hor(row, 10, 0);
                lv_obj_set_style_bg_color(row, lv_color_hex(0xF5F5F5), 0);
                lv_obj_set_style_shadow_width(row, 0, 0);

                lv_obj_t *nm = lv_label_create(row);
                lv_label_set_text(nm, m->search_name[i]);
                lv_obj_set_style_text_font(nm, g_cjk_font, 0);
                lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

                lv_obj_add_event_cb(row, on_result_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            }
        }
    } else {
        lv_obj_t *hint = lv_label_create(results);
        lv_label_set_text(hint, tr(STR_WEATHER_SEARCH_HINT));
        lv_obj_set_style_text_font(hint, g_cjk_font, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xBBBBBB), 0);
        lv_obj_center(hint);
    }

    /* ── keyboard + IME (hidden by default) ── */
    lv_obj_t *kb = lv_keyboard_create(page.screen);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    /* 默认键盘高度=屏高50%(160px),候选栏浮在其上会盖到搜索框;
     * 压到 140px 让「键盘+候选栏」整体落在搜索框下方,不遮挡输入框。 */
    lv_obj_set_height(kb, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);

    lv_obj_t *ime = lv_ime_pinyin_create(page.screen);
    if (g_cjk_font) lv_obj_set_style_text_font(ime, g_cjk_font, 0);
    lv_ime_pinyin_set_keyboard(ime, kb);
    lv_ime_pinyin_set_dict(ime, (lv_pinyin_dict_t *)g_pinyin_zh_cn_dict);   /* 简体优先 */
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(cand, LV_PCT(100), 40);
    lv_obj_align_to(cand, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);

    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_kb_map_lc, s_kb_ctrl_lc);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, on_kb_lang, LV_EVENT_VALUE_CHANGED, ime);
    s_kb_en_mode = false;

    ctx->kb = kb;
    ctx->ime = ime;
    ctx->cand = cand;

    g_active_city_ctx = ctx;
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_add_event_cb(indev, indev_press_filter, LV_EVENT_PRESSED, NULL);

    return page.screen;
}

void weather_view_city_init_registry(struct WeatherApp *app) {
    PAGE_REGISTE(app, PAGE_CITY, build_city_page);
}
