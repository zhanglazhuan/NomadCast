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
#include "lv_home_indicator.h"
#include "lang.h"

extern RadioApp g_radio_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    lv_obj_t   *title_label;   /* station name */
    lv_obj_t   *sub_label;     /* region · codec */
    lv_obj_t   *pp_label;      /* play/pause glyph */
    lv_obj_t   *fav_label;     /* favorite toggle label */
    lv_obj_t   *fav_btn;
    int         shown_src;     /* last list source rendered */
    int         shown_idx;     /* last station index rendered */
    lv_timer_t *timer;
} RadioPlayingCtx;

/* The playing station may come from either the browse list or favorites. */
static radio_station_t *cur_station(RadioModel *m) {
    if (!m || m->current < 0) return NULL;
    if (m->play_source == RADIO_SRC_FAVORITES) {
        if (m->current >= m->favorite_count) return NULL;
        return &m->favorites[m->current];
    }
    if (m->current >= m->station_count) return NULL;
    return &m->stations[m->current];
}

static void refresh_playing(RadioPlayingCtx *ctx) {
    if (!ctx) return;
    RadioModel *m = g_radio_app.model;

    bool playing = audio_player_is_playing();
    if (ctx->pp_label && lv_obj_is_valid(ctx->pp_label)) {
        lv_label_set_text(ctx->pp_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    radio_station_t *s = cur_station(m);
    int src = m ? m->play_source : -1;
    int idx = m ? m->current : -1;

    if ((src != ctx->shown_src || idx != ctx->shown_idx) || !s) {
        ctx->shown_src = src;
        ctx->shown_idx = idx;

        const char *name = s ? s->name : tr(STR_NOTHING_PLAYING);
        if (ctx->title_label && lv_obj_is_valid(ctx->title_label))
            lv_label_set_text(ctx->title_label, name);

        char sub[96];
        if (s) {
            if (s->hls)
                snprintf(sub, sizeof(sub), "%s HLS", s->region_name);
            else if (s->bitrate > 0 && s->codec[0])
                snprintf(sub, sizeof(sub), "%s %s %dkbps", s->region_name, s->codec, s->bitrate);
            else
                snprintf(sub, sizeof(sub), "%s", s->region_name);
        } else {
            sub[0] = '\0';
        }
        if (ctx->sub_label && lv_obj_is_valid(ctx->sub_label))
            lv_label_set_text(ctx->sub_label, sub);

        bool fav = s ? radio_model_is_favorite(&g_radio_app, s->uuid) : false;
        if (ctx->fav_label && lv_obj_is_valid(ctx->fav_label))
            lv_label_set_text(ctx->fav_label, tr(fav ? STR_RADIO_FAVORITED : STR_RADIO_FAVORITE));
        if (ctx->fav_btn && lv_obj_is_valid(ctx->fav_btn))
            lv_obj_set_style_bg_color(ctx->fav_btn,
                                      fav ? lv_color_hex(0xFFEBEE) : lv_color_hex(0xEEEEEE), 0);
    }
}

static void playing_timer_cb(lv_timer_t *timer) {
    RadioPlayingCtx *ctx = (RadioPlayingCtx *)lv_timer_get_user_data(timer);
    refresh_playing(ctx);
}

static void playing_ctx_cleanup(lv_event_t *e) {
    RadioPlayingCtx *ctx = (RadioPlayingCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    free(ctx);
}

static void on_play_pause(lv_event_t *e) { (void)e; radio_controller_toggle(&g_radio_app); }
static void on_prev_station(lv_event_t *e) { (void)e; radio_controller_prev(&g_radio_app); }
static void on_next_station(lv_event_t *e) { (void)e; radio_controller_next(&g_radio_app); }

static void on_fav_click(lv_event_t *e) {
    (void)e;
    radio_controller_toggle_favorite(&g_radio_app);
}

static lv_obj_t *build_playing_page(struct RadioApp *app, void *user_data) {
    (void)user_data;
    RadioModel *m = app->model;
    if (m) m->current_page = PAGE_PLAYING;

    radio_station_t *s = cur_station(m);
    const char *tname = s ? s->name : tr(STR_NOTHING_PLAYING);

    Page page = lv_page_create(NULL, true, page_navigator_navigate_back, &app->view->page_nav);
    lv_home_indicator_create(page.screen);
    lv_obj_t *main = page.container;
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_top(main, 6, 0);
    lv_obj_set_style_pad_bottom(main, 58, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(main, LV_DIR_NONE);

    /* cover */
    lv_obj_t *cover = lv_obj_create(main);
    lv_obj_set_size(cover, 52, 52);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 10, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ci = lv_label_create(cover);
    lv_label_set_text(ci, LV_SYMBOL_AUDIO);
    lv_obj_center(ci);
    lv_obj_set_style_text_color(ci, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ci, &lv_font_montserrat_20, 0);

    /* LIVE badge + favorite toggle on one row */
    lv_obj_t *top_row = lv_obj_create(main);
    lv_obj_set_size(top_row, LV_PCT(100), 30);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_set_style_pad_hor(top_row, 24, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_top(top_row, 8, 0);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *live = lv_label_create(top_row);
    lv_label_set_text(live, tr(STR_RADIO_LIVE));
    lv_obj_set_style_text_color(live, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_text_font(live, g_cjk_font, 0);

    lv_obj_t *fav_btn = lv_button_create(top_row);
    lv_obj_set_size(fav_btn, 92, 28);
    lv_obj_set_style_radius(fav_btn, 14, 0);
    lv_obj_set_style_border_width(fav_btn, 0, 0);
    lv_obj_set_style_shadow_width(fav_btn, 0, 0);
    lv_obj_set_style_bg_color(fav_btn, lv_color_hex(0xEEEEEE), 0);
    lv_obj_t *fav_label = lv_label_create(fav_btn);
    lv_label_set_text(fav_label, tr(STR_RADIO_FAVORITE));
    lv_obj_center(fav_label);
    lv_obj_set_style_text_font(fav_label, g_cjk_font, 0);
    lv_obj_set_style_text_color(fav_label, lv_color_hex(0xE53935), 0);
    lv_obj_add_event_cb(fav_btn, on_fav_click, LV_EVENT_CLICKED, NULL);

    /* station name */
    lv_obj_t *title = lv_label_create(main);
    lv_label_set_text(title, tname);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LV_PCT(85));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, g_cjk_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);
    lv_obj_set_style_margin_top(title, 4, 0);

    /* region · codec subtitle */
    lv_obj_t *sub = lv_label_create(main);
    lv_label_set_text(sub, "");
    lv_obj_set_width(sub, LV_PCT(85));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sub, g_cjk_font, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x999999), 0);

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

    RadioPlayingCtx *ctx = (RadioPlayingCtx *)calloc(1, sizeof(RadioPlayingCtx));
    ctx->title_label = title;
    ctx->sub_label   = sub;
    ctx->pp_label    = lc;
    ctx->fav_label   = fav_label;
    ctx->fav_btn     = fav_btn;
    ctx->shown_src   = -1;
    ctx->shown_idx   = -1;
    ctx->timer = lv_timer_create(playing_timer_cb, 500, ctx);
    lv_obj_add_event_cb(page.screen, playing_ctx_cleanup, LV_EVENT_DELETE, ctx);

    /* Prime labels immediately (before the first 500ms tick). */
    refresh_playing(ctx);

    return page.screen;
}

void radio_view_playing_init_registry(struct RadioApp *app) {
    PAGE_REGISTE(app, PAGE_PLAYING, build_playing_page);
}
