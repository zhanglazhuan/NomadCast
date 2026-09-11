#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_playing.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "audio_player.h"
#include "lv_page.h"
#include "lang.h"

extern RadioApp g_radio_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    lv_obj_t   *title_label;   /* station name */
    lv_obj_t   *pp_label;      /* play/pause glyph */
    int         shown_idx;     /* last station index rendered into title_label */
    lv_timer_t *timer;
} RadioPlayingCtx;

static const char *current_station_name(void) {
    RadioModel *m = g_radio_app.model;
    if (!m || m->current < 0 || m->current >= m->count) return NULL;
    return m->stations[m->current].name;
}

static void playing_timer_cb(lv_timer_t *timer) {
    RadioPlayingCtx *ctx = (RadioPlayingCtx *)lv_timer_get_user_data(timer);
    if (!ctx) return;

    bool playing = audio_player_is_playing();
    if (ctx->pp_label && lv_obj_is_valid(ctx->pp_label)) {
        lv_label_set_text(ctx->pp_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    RadioModel *m = g_radio_app.model;
    int idx = (m && m->current >= 0 && m->current < m->count) ? m->current : -1;
    if (idx != ctx->shown_idx && ctx->title_label && lv_obj_is_valid(ctx->title_label)) {
        ctx->shown_idx = idx;
        const char *name = (idx >= 0) ? m->stations[idx].name : tr(STR_NOTHING_PLAYING);
        lv_label_set_text(ctx->title_label, name);
    }
}

static void playing_ctx_cleanup(lv_event_t *e) {
    RadioPlayingCtx *ctx = (RadioPlayingCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    free(ctx);
}

static void on_play_pause(lv_event_t *e) {
    (void)e;
    radio_controller_toggle(&g_radio_app);
}

static void on_prev_station(lv_event_t *e) {
    (void)e;
    radio_controller_prev(&g_radio_app);
}

static void on_next_station(lv_event_t *e) {
    (void)e;
    radio_controller_next(&g_radio_app);
}

static lv_obj_t *build_playing_page(struct RadioApp *app, void *user_data) {
    (void)user_data;

    if (app->model) app->model->current_page = PAGE_PLAYING;

    const char *name = current_station_name();
    const char *tname = name ? name : tr(STR_NOTHING_PLAYING);

    Page page = lv_page_create(NULL, true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t *main = page.container;
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(main, LV_DIR_NONE);

    /* cover */
    lv_obj_t *cover = lv_obj_create(main);
    lv_obj_set_size(cover, 72, 72);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 12, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ci = lv_label_create(cover);
    lv_label_set_text(ci, LV_SYMBOL_AUDIO);
    lv_obj_center(ci);
    lv_obj_set_style_text_color(ci, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ci, g_cjk_font, 0);

    /* LIVE badge */
    lv_obj_t *live = lv_label_create(main);
    lv_label_set_text(live, tr(STR_RADIO_LIVE));
    lv_obj_set_style_text_color(live, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_text_font(live, g_cjk_font, 0);
    lv_obj_set_style_margin_top(live, 8, 0);

    /* station name (auto-scroll if too long) */
    lv_obj_t *title = lv_label_create(main);
    lv_label_set_text(title, tname);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LV_PCT(85));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, g_cjk_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);
    lv_obj_set_style_margin_top(title, 4, 0);

    /* controls row */
    lv_obj_t *controls = lv_obj_create(main);
    lv_obj_set_size(controls, LV_PCT(85), 56);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_top(controls, 16, 0);
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* prev */
    lv_obj_t *bp = lv_button_create(controls);
    lv_obj_set_size(bp, 48, 48);
    lv_obj_set_style_radius(bp, 24, 0);
    lv_obj_set_style_bg_color(bp, lv_color_hex(0xEEEEEE), 0);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, LV_SYMBOL_PREV);
    lv_obj_center(lp);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lp, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(bp, on_prev_station, LV_EVENT_CLICKED, NULL);

    /* play / pause */
    lv_obj_t *bc = lv_button_create(controls);
    lv_obj_set_size(bc, 56, 56);
    lv_obj_set_style_radius(bc, 28, 0);
    lv_obj_set_style_bg_color(bc, lv_color_hex(0x1E88E5), 0);
    lv_obj_t *lc = lv_label_create(bc);
    lv_label_set_text(lc, audio_player_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_center(lc);
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lc, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(bc, on_play_pause, LV_EVENT_CLICKED, NULL);

    /* next */
    lv_obj_t *bn = lv_button_create(controls);
    lv_obj_set_size(bn, 48, 48);
    lv_obj_set_style_radius(bn, 24, 0);
    lv_obj_set_style_bg_color(bn, lv_color_hex(0xEEEEEE), 0);
    lv_obj_t *ln = lv_label_create(bn);
    lv_label_set_text(ln, LV_SYMBOL_NEXT);
    lv_obj_center(ln);
    lv_obj_set_style_text_font(ln, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ln, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(bn, on_next_station, LV_EVENT_CLICKED, NULL);

    /* context + timer */
    RadioPlayingCtx *ctx = (RadioPlayingCtx *)calloc(1, sizeof(RadioPlayingCtx));
    ctx->title_label = title;
    ctx->pp_label    = lc;
    ctx->shown_idx   = (app->model && app->model->current >= 0 && app->model->current < app->model->count)
                       ? app->model->current : -1;
    ctx->timer = lv_timer_create(playing_timer_cb, 500, ctx);
    lv_obj_add_event_cb(page.screen, playing_ctx_cleanup, LV_EVENT_DELETE, ctx);

    return page.screen;
}

void radio_view_playing_init_registry(struct RadioApp *app) {
    PAGE_REGISTE(app, PAGE_PLAYING, build_playing_page);
}
