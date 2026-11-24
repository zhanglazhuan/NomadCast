#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_browse.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lang.h"
#include "pinyin_zh_cn.h"

extern RadioApp g_radio_app;
extern const lv_font_t *g_cjk_font;

/* Defined in apps/settings/hal_esp.c — shared across apps. */
bool hal_wifi_is_connected(void);

typedef struct {
    lv_obj_t *textarea;
    lv_obj_t *kb;
    lv_obj_t *ime;
    lv_obj_t *cand;
    lv_obj_t *body;            /* scrollable region list / status container */
} BrowseCtx;

static BrowseCtx *g_active_browse_ctx = NULL;

static void browse_render_body(BrowseCtx *ctx);

/* ── 键盘显隐 ─────────────────────────────────────────────────────────── */

static bool pt_in_area(const lv_point_t *pt, const lv_area_t *a) {
    return pt->x >= a->x1 && pt->x <= a->x2 && pt->y >= a->y1 && pt->y <= a->y2;
}

static void show_keyboard(BrowseCtx *ctx) {
    if (!ctx || !ctx->kb || !ctx->textarea) return;
    lv_obj_clear_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(ctx->textarea, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(ctx->kb, ctx->textarea);
}

static void hide_keyboard(BrowseCtx *ctx) {
    if (!ctx || !ctx->kb) return;
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    if (ctx->textarea) lv_obj_clear_state(ctx->textarea, LV_STATE_FOCUSED);
    if (ctx->cand) lv_obj_add_flag(ctx->cand, LV_OBJ_FLAG_HIDDEN);
}

static void indev_press_filter(lv_event_t *e) {
    BrowseCtx *ctx = g_active_browse_ctx;
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

/* ── 键盘中英切换 ──────────────────────────────────────────────────────── */

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

/* ── 事件处理 ──────────────────────────────────────────────────────────── */

static bool matches_query(const char *name, const char *q) {
    if (!q || !q[0]) return true;
    /* Naive case-insensitive substring match. */
    size_t qlen = strlen(q);
    for (const char *p = name; *p; p++) {
        for (size_t i = 0; i < qlen; i++) {
            char a = p[i], b = q[i];
            if (!p[i]) return false;
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            if (i == qlen - 1) return true;
        }
    }
    return false;
}

static void on_region_click(lv_event_t *e) {
    const char *key = (const char *)lv_event_get_user_data(e);
    if (!key) return;
    RadioModel *m = g_radio_app.model;
    if (!m) return;
    /* Find the region name to store as page title. */
    const char *name = key;
    for (int i = 0; i < m->region_count; i++) {
        if (strcmp(m->regions[i].key, key) == 0) { name = m->regions[i].name; break; }
    }
    radio_controller_load_stations(&g_radio_app, key, name);
    radio_nav_push(&g_radio_app, PAGE_BROWSE, PAGE_STATIONS);
}

static void on_search_changed(lv_event_t *e) {
    BrowseCtx *ctx = g_active_browse_ctx;
    if (!ctx) return;
    RadioModel *m = g_radio_app.model;
    if (!m) return;
    const char *txt = lv_textarea_get_text(ctx->textarea);
    strncpy(m->search_query, txt, sizeof(m->search_query) - 1);
    m->search_query[sizeof(m->search_query) - 1] = '\0';
    /* Re-render just the list (keep the keyboard + country selector intact). */
    if (ctx->body) {
        lv_obj_clean(ctx->body);
        /* render_body is defined below; forward call through a helper. */
        browse_render_body(ctx);
    }
}

static void on_retry(lv_event_t *e) {
    (void)e;
    radio_controller_load_regions(&g_radio_app);
}

/* ── 列表/状态渲染 ─────────────────────────────────────────────────────── */

static void add_status_label(BrowseCtx *ctx, const char *text) {
    lv_obj_t *lbl = lv_label_create(ctx->body);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
    lv_obj_set_width(lbl, LV_PCT(90));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);
}

void browse_render_body(BrowseCtx *ctx) {
    if (!ctx || !ctx->body) return;
    lv_obj_clean(ctx->body);

    RadioModel *m = g_radio_app.model;
    if (!m) return;

    if (m->region_count == 0) {
        if (m->busy) {
            add_status_label(ctx, tr(STR_RADIO_LOADING));
        } else if (!hal_wifi_is_connected()) {
            radio_view_wifi_offline(ctx->body);
        } else if (m->error[0]) {
            add_status_label(ctx, m->error);
            lv_obj_t *btn = lv_button_create(ctx->body);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_size(btn, 140, 40);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E88E5), 0);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
            lv_obj_t *bl = lv_label_create(btn);
            lv_label_set_text(bl, tr(STR_RADIO_RETRY));
            lv_obj_center(bl);
            lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(bl, g_cjk_font, 0);
            lv_obj_add_event_cb(btn, on_retry, LV_EVENT_CLICKED, NULL);
        } else {
            add_status_label(ctx, tr(STR_RADIO_NO_REGIONS));
        }
        return;
    }

    int shown = 0;
    for (int i = 0; i < m->region_count; i++) {
        const radio_region_t *r = &m->regions[i];
        if (!matches_query(r->name, m->search_query)) continue;

        lv_obj_t *row = lv_button_create(ctx->body);
        lv_obj_set_size(row, LV_PCT(100), 44);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, r->name);
        lv_obj_set_style_text_font(nm, g_cjk_font, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0x333333), 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, 150);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 16, 0);

        char cnt[32];
        snprintf(cnt, sizeof(cnt), tr(STR_RADIO_STATION_COUNT), r->count);
        lv_obj_t *cl = lv_label_create(row);
        lv_label_set_text(cl, cnt);
        lv_obj_set_style_text_font(cl, g_cjk_font, 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(0x999999), 0);
        lv_obj_align(cl, LV_ALIGN_RIGHT_MID, -16, 0);

        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        /* Store the region key (owned by the model — persists for the app's life). */
        lv_obj_add_event_cb(row, on_region_click, LV_EVENT_CLICKED, (void *)r->key);
        shown++;
    }

    if (shown == 0) {
        add_status_label(ctx, tr(STR_RADIO_NO_REGIONS));
    }
}

static void ctx_cleanup_cb(lv_event_t *e) {
    BrowseCtx *ctx = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_remove_event_cb_with_user_data(indev, indev_press_filter, NULL);
    g_active_browse_ctx = NULL;
    if (ctx) free(ctx);
}

static lv_obj_t *build_browse_page(struct RadioApp *app, void *user_data) {
    (void)user_data;
    RadioModel *m = app->model;
    if (m) m->current_page = PAGE_BROWSE;

    Page page = lv_page_create(tr(STR_APP_RADIO), false, NULL, NULL);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

    BrowseCtx *ctx = (BrowseCtx *)calloc(1, sizeof(BrowseCtx));
    lv_obj_add_event_cb(page.screen, ctx_cleanup_cb, LV_EVENT_DELETE, ctx);

    /* ── search bar ── */
    lv_obj_t *search_bar = lv_obj_create(cont);
    lv_obj_set_size(search_bar, LV_PCT(100), 44);
    lv_obj_set_style_border_width(search_bar, 0, 0);
    lv_obj_set_style_pad_all(search_bar, 6, 0);
    lv_obj_set_style_pad_bottom(search_bar, 8, 0);

    lv_obj_t *ta = lv_textarea_create(search_bar);
    lv_obj_set_size(ta, LV_PCT(100), 32);
    lv_textarea_set_placeholder_text(ta, tr(STR_RADIO_SEARCH_HINT));
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 4, 0);
    lv_textarea_set_one_line(ta, true);
    if (m && m->search_query[0]) lv_textarea_set_text(ta, m->search_query);
    ctx->textarea = ta;
    lv_obj_add_event_cb(ta, on_search_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── region list body ── */
    lv_obj_t *body = lv_obj_create(cont);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    ctx->body = body;

    /* ── keyboard + IME (hidden by default) ── */
    lv_obj_t *kb = lv_keyboard_create(page.screen);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(kb, 140);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);

    lv_obj_t *ime = lv_ime_pinyin_create(page.screen);
    if (g_cjk_font) lv_obj_set_style_text_font(ime, g_cjk_font, 0);
    lv_ime_pinyin_set_keyboard(ime, kb);
    lv_ime_pinyin_set_dict(ime, (lv_pinyin_dict_t *)g_pinyin_zh_cn_dict);
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

    ctx->kb  = kb;
    ctx->ime = ime;
    ctx->cand = cand;

    /* Populate the list now. */
    browse_render_body(ctx);

    g_active_browse_ctx = ctx;
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_add_event_cb(indev, indev_press_filter, LV_EVENT_PRESSED, NULL);

    radio_view_create_bottom_tab_bar(page.screen, RADIO_TAB_BROWSE);

    return page.screen;
}

void radio_view_browse_init_registry(struct RadioApp *app) {
    PAGE_REGISTE(app, PAGE_BROWSE, build_browse_page);
}
