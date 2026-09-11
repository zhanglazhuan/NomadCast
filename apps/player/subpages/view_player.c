#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "view_player.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "audio_player.h"
#include "lv_page.h"
#include "lang.h"

extern PlayerApp g_player_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    lv_obj_t   *pbar;
    lv_obj_t   *time_label;
    lv_obj_t   *total_label;
    lv_obj_t   *pp_label;
    int         duration;
    lv_timer_t *pos_timer;
} PlayerCtx;

static void fmt_time(int sec, char *buf, int sz) {
    if (sec < 0) sec = 0;
    snprintf(buf, sz, "%d:%02d", sec / 60, sec % 60);
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void player_pos_timer_cb(lv_timer_t *timer) {
    PlayerCtx *ctx = (PlayerCtx *)lv_timer_get_user_data(timer);
    if (!ctx) return;

    bool playing = audio_player_is_playing();
    if (ctx->pp_label && lv_obj_is_valid(ctx->pp_label)) {
        lv_label_set_text(ctx->pp_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (!playing) return;

    int pos = audio_player_get_position_sec();
    int real_dur = audio_player_get_duration_sec();
    if (real_dur > 0 && real_dur != ctx->duration) {
        ctx->duration = real_dur;
        if (lv_obj_is_valid(ctx->pbar)) lv_bar_set_range(ctx->pbar, 0, real_dur);
        if (lv_obj_is_valid(ctx->total_label)) {
            char db[16]; fmt_time(real_dur, db, sizeof(db));
            lv_label_set_text(ctx->total_label, db);
        }
    }
    if (ctx->duration > 0 && pos > ctx->duration) pos = ctx->duration;
    if (lv_obj_is_valid(ctx->pbar)) lv_bar_set_value(ctx->pbar, pos, LV_ANIM_OFF);

    char buf[16]; fmt_time(pos, buf, sizeof(buf));
    if (lv_obj_is_valid(ctx->time_label)) lv_label_set_text(ctx->time_label, buf);
}

static void player_ctx_cleanup(lv_event_t *e) {
    PlayerCtx *ctx = (PlayerCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->pos_timer) { lv_timer_del(ctx->pos_timer); ctx->pos_timer = NULL; }
    free(ctx);
}

static void on_play_pause(lv_event_t *e) {
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    bool playing = audio_player_is_playing();
    audio_player_pause(playing);   /* toggle: playing → pause, paused → resume */
    if (label && lv_obj_is_valid(label)) {
        lv_label_set_text(label, playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

static lv_obj_t *build_player_page(struct PlayerApp *app, void *user_data) {
    (void)user_data;

    const char *full = app->model->current_file;
    const char *tname = full[0] ? basename_of(full) : tr(STR_NOTHING_PLAYING);

    Page page = lv_page_create(NULL, true, page_navigator_navigate_back, &app->view->page_nav);
    int container_h = lv_display_get_vertical_resolution(lv_display_get_default())
                      - LV_STATUS_BAR_HEIGHT - LV_PAGE_HEADER_HEIGHT - PLAYER_TAB_BAR_HEIGHT;
    lv_obj_set_height(page.container, container_h);

    lv_obj_t *main = page.container;
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_bottom(main, 16, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(main, LV_DIR_NONE);

    /* cover */
    lv_obj_t *cover = lv_obj_create(main);
    lv_obj_set_size(cover, 60, 60);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0xFF7043), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 8, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ci = lv_label_create(cover);
    lv_label_set_text(ci, LV_SYMBOL_AUDIO);
    lv_obj_center(ci);
    lv_obj_set_style_text_color(ci, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ci, g_cjk_font, 0);

    /* title (marquee if too long) */
    {
        lv_point_t tsz;
        lv_text_get_size(&tsz, tname, g_cjk_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t title_w = screen_w * 85 / 100;

        lv_obj_t *tn_box = lv_obj_create(main);
        lv_obj_set_size(tn_box, LV_PCT(85), tsz.y > 0 ? tsz.y : 24);
        lv_obj_set_style_pad_all(tn_box, 0, 0);
        lv_obj_set_style_border_width(tn_box, 0, 0);
        lv_obj_set_style_bg_opa(tn_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_margin_top(tn_box, 8, 0);
        lv_obj_clear_flag(tn_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tn_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        lv_obj_t *tn = lv_label_create(tn_box);
        lv_label_set_text(tn, tname);
        lv_obj_set_style_text_font(tn, g_cjk_font, 0);

        if (tsz.x > title_w) {
            lv_obj_align(tn, LV_ALIGN_LEFT_MID, 0, 0);
            int32_t scroll_end = title_w - tsz.x;
            uint32_t dur = (uint32_t)(-scroll_end) * 12;
            if (dur < 800) dur = 800;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, tn);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
            lv_anim_set_values(&a, 0, scroll_end);
            lv_anim_set_duration(&a, dur);
            lv_anim_set_repeat_delay(&a, 2000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        } else {
            lv_obj_align(tn, LV_ALIGN_CENTER, 0, 0);
        }
    }

    int duration = audio_player_get_duration_sec();

    PlayerCtx *pctx = (PlayerCtx *)calloc(1, sizeof(PlayerCtx));
    pctx->duration = duration;

    /* progress bar */
    lv_obj_t *pbar = lv_bar_create(main);
    lv_obj_set_size(pbar, LV_PCT(85), 4);
    lv_obj_set_style_pad_top(pbar, 12, 0);
    lv_bar_set_range(pbar, 0, duration > 0 ? duration : 1);
    lv_bar_set_value(pbar, audio_player_get_position_sec(), LV_ANIM_OFF);
    pctx->pbar = pbar;

    /* time labels */
    lv_obj_t *ptime = lv_obj_create(main);
    lv_obj_set_size(ptime, LV_PCT(85), 18);
    lv_obj_set_style_border_width(ptime, 0, 0);
    lv_obj_set_style_bg_opa(ptime, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ptime, 0, 0);
    lv_obj_clear_flag(ptime, LV_OBJ_FLAG_SCROLLABLE);

    char tbuf[16]; fmt_time(audio_player_get_position_sec(), tbuf, sizeof(tbuf));
    lv_obj_t *ct = lv_label_create(ptime);
    lv_label_set_text(ct, tbuf);
    lv_obj_align(ct, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(ct, g_cjk_font, 0);
    pctx->time_label = ct;

    char dbuf[16]; fmt_time(duration, dbuf, sizeof(dbuf));
    lv_obj_t *dt = lv_label_create(ptime);
    lv_label_set_text(dt, dbuf);
    lv_obj_align(dt, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(dt, g_cjk_font, 0);
    pctx->total_label = dt;

    pctx->pos_timer = lv_timer_create(player_pos_timer_cb, 500, pctx);
    lv_obj_add_event_cb(page.screen, player_ctx_cleanup, LV_EVENT_DELETE, pctx);

    /* play / pause button */
    lv_obj_t *b = lv_button_create(main);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xFF7043), 0);
    lv_obj_set_style_margin_top(b, 8, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, audio_player_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    pctx->pp_label = l;
    lv_obj_add_event_cb(b, on_play_pause, LV_EVENT_CLICKED, l);

    player_view_create_bottom_tab_bar(page.screen, PLAYER_TAB_PLAYER);

    return page.screen;
}

void player_view_player_init_registry(struct PlayerApp *app) {
    PAGE_REGISTE(app, PAGE_PLAYER, build_player_page);
}
