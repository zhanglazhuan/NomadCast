/**
 * @file view_search.c
 * @brief Search page — triggers async backend search, navigates to results
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_search.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"

extern PodcastApp g_podcast_app;

typedef struct {
    lv_obj_t *textarea;
} SearchPageCtx;

static void do_search(const char *query) {
    if (!query || !query[0]) return;
    podcast_model_add_search_history(&g_podcast_app, query);
    /* Initiate async search */
    podcast_controller_search(&g_podcast_app, query);
    /* Navigate to results — results page will poll for completion */
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_SEARCH, PAGE_SEARCH_RESULTS, NULL);
}

static void on_enter(lv_event_t *e) {
    SearchPageCtx *ctx = lv_event_get_user_data(e);
    const char *query = lv_textarea_get_text(ctx->textarea);
    do_search(query);
}

static void on_history_clicked(lv_event_t *e) {
    const char *query = (const char *)lv_event_get_user_data(e);
    if (query) do_search(query);
}

static void on_focus_timer(lv_timer_t *timer) {
    lv_obj_t *ta = lv_timer_get_user_data(timer);
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(ta);
}

static void ctx_cleanup_cb(lv_event_t *e) {
    SearchPageCtx *ctx = lv_event_get_user_data(e);
    if (ctx) {
        free(ctx);
        g_podcast_app.view->page_nav.nav_ctx = NULL;
    }
}

/* ── 键盘中英切换 (左下角 中/EN 键) ──────────────────────────────────────────
 * 把默认小写键盘左下角的“收起键盘”键换成“中/EN”:
 *   中 (默认) = 显示拼音候选、可输入中文;EN = 隐藏候选、纯英文。
 * lv_keyboard_set_map 是全局覆盖且大小写切换也沿用它,所以自定义图会一直生效;
 * 键改名后 lv_ime_pinyin 不再把它当 K9 切换键,默认处理器也不再当“收起”键。 */
#define KB_LANG_BTN "中/EN"
#define KB_POP(w)   (LV_BUTTONMATRIX_CTRL_POPOVER | (w))

static bool s_kb_en_mode = false;   /* false=中文(候选开), true=英文(候选关) */

/* 复制自 LVGL 默认小写键盘图,仅左下角 LV_SYMBOL_KEYBOARD → 中/EN */
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

/* 接管键盘按键:中/EN 键切换模式;其余交默认处理;英文模式压制候选栏。
 * (替换掉默认 lv_keyboard_def_event_cb,顺序变为 ime cb → 本回调) */
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
        return;   /* 不落到默认处理 → 不会把 "中/EN" 输入到文本框 */
    }

    lv_keyboard_def_event_cb(e);   /* 默认键盘行为:输入/退格/大小写切换/OK 等 */

    if (s_kb_en_mode) {            /* 英文模式:即便 IME 弹了候选也立即隐藏 */
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
        if (cand) lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *build_search_page(struct PodcastApp *app, void *user_data) {
    (void)user_data;
    Page page = lv_page_create("Search", true, page_navigator_navigate_back, &app->view->page_nav);

    if (app->view->page_nav.nav_ctx) free(app->view->page_nav.nav_ctx);
    SearchPageCtx *ctx = (SearchPageCtx *)calloc(1, sizeof(SearchPageCtx));
    app->view->page_nav.nav_ctx = ctx;
    lv_obj_add_event_cb(page.screen, ctx_cleanup_cb, LV_EVENT_DELETE, ctx);

    /* Search bar */
    lv_obj_t *search_bar = lv_obj_create(page.container);
    lv_obj_set_size(search_bar, LV_PCT(100), 44);
    lv_obj_set_style_border_width(search_bar, 0, 0);
    lv_obj_set_style_pad_all(search_bar, 6, 0);

    lv_obj_t *ta = lv_textarea_create(search_bar);
    lv_obj_set_size(ta, LV_PCT(100), 32);
    lv_textarea_set_placeholder_text(ta, "Search podcasts...");
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 4, 0);
    lv_textarea_set_one_line(ta, true);
    ctx->textarea = ta;
    lv_obj_add_event_cb(ta, on_enter, LV_EVENT_READY, ctx);

    /* History */
    lv_obj_t *history = lv_obj_create(page.container);
    lv_obj_set_size(history, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(history, 1);
    lv_obj_set_flex_flow(history, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(history, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_border_width(history, 0, 0);
    lv_obj_set_style_pad_all(history, 8, 0);
    lv_obj_set_style_pad_row(history, 6, 0);
    lv_obj_set_style_pad_column(history, 6, 0);

    int hist_count = podcast_model_get_search_history_count(app);
    for (int i = 0; i < hist_count && i < 10; i++) {
        const char *item = podcast_model_get_search_history_item(app, i);
        if (!item) continue;

        lv_obj_t *btn = lv_button_create(history);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_height(btn, 28);
        lv_obj_set_style_pad_hor(btn, 12, 0);
        lv_obj_set_style_pad_ver(btn, 2, 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(btn, 0, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, item);
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_text_font(label, g_cjk_font, 0);

        lv_obj_add_event_cb(btn, on_history_clicked, LV_EVENT_CLICKED, (void *)item);
    }

    /* Keyboard (floating, 脱离 flex 布局避免溢出) */
    lv_obj_t *kb = lv_keyboard_create(page.screen);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);

    /* 拼音输入法:打字母拼音 → 候选栏浮在键盘上方显示汉字,点选上屏。
     * 候选字用 CJK 字体渲染 (英文搜索不选候选、直接空格/回车即可)。 */
    lv_obj_t *ime = lv_ime_pinyin_create(page.screen);
    if (g_cjk_font) lv_obj_set_style_text_font(ime, g_cjk_font, 0);
    lv_ime_pinyin_set_keyboard(ime, kb);

    lv_keyboard_set_textarea(kb, ta);

    /* 候选栏默认只有屏高 5% 且随父布局排布 → 放大到 40px 并浮在键盘正上方,
     * 保证敲拼音时清晰可见 (get_cand_panel 必须在 set_keyboard 之后调用)。 */
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(cand, LV_PCT(100), 40);
    lv_obj_align_to(cand, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);

    /* 左下角键改成"中/EN"并接管键盘回调 (在 set_keyboard 之后,让本回调排在 ime 之后) */
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_kb_map_lc, s_kb_ctrl_lc);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, on_kb_lang, LV_EVENT_VALUE_CHANGED, ime);
    s_kb_en_mode = false;   /* 每次进页面默认中文 */

    lv_timer_t *focus_timer = lv_timer_create(on_focus_timer, 100, ta);
    lv_timer_set_repeat_count(focus_timer, 1);

    return page.screen;
}

void podcast_view_search_init_registry(struct PodcastApp *app) {
    PAGE_REGISTE(app, PAGE_SEARCH, build_search_page);
}
