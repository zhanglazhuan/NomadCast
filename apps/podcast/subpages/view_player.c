/**
 * @file view_player.c
 * @brief 播放页 — 封面 + 进度条 + 控制按钮 + 定时关机弹窗
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_player.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "../hal.h"
#include "../cache.h"
#include "lv_bottom_sheet.h"
#include "lv_num_input.h"
#include "lv_page.h"

extern PodcastApp g_podcast_app;

/* ── Player position polling ───────────────────────────────────────────── */

typedef struct {
    lv_obj_t   *pbar;
    lv_obj_t   *time_label;
    lv_obj_t   *pp_label;       /* play/pause button glyph — synced to model state */
    int         duration;
    int         episode_id;
    int         channel_id;
    int         last_saved;     /* debounce: last saved position */
    lv_timer_t *pos_timer;
} PlayerCtx;

static void player_pos_timer_cb(lv_timer_t *timer) {
    PlayerCtx *ctx = (PlayerCtx *)lv_timer_get_user_data(timer);
    if (!ctx) return;

    /* Keep the play/pause glyph in sync with playback state, which may be
     * toggled by the physical key from another thread. */
    if (ctx->pp_label && lv_obj_is_valid(ctx->pp_label)) {
        lv_label_set_text(ctx->pp_label,
            podcast_model_is_playing(&g_podcast_app) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    if (!podcast_model_is_playing(&g_podcast_app)) return;

    int pos = hal_audio_get_position_sec();
    if (pos > ctx->duration && ctx->duration > 0) pos = ctx->duration;

    if (lv_obj_is_valid(ctx->pbar))
        lv_bar_set_value(ctx->pbar, pos, LV_ANIM_OFF);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", pos / 60, pos % 60);
    if (lv_obj_is_valid(ctx->time_label))
        lv_label_set_text(ctx->time_label, buf);

    /* Periodic save: every 5 seconds */
    if (pos > 0 && (pos - ctx->last_saved >= 5 || pos < ctx->last_saved)) {
        ctx->last_saved = pos;
        cache_playback_set_position(ctx->episode_id, ctx->channel_id,
                                    pos, ctx->duration);
    }
}

static void player_ctx_cleanup(lv_event_t *e) {
    PlayerCtx *ctx = (PlayerCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    /* Final save on page close */
    if (ctx->episode_id > 0) {
        int pos = hal_audio_get_position_sec();
        if (pos > 0)
            cache_playback_set_position(ctx->episode_id, ctx->channel_id,
                                        pos, ctx->duration);
    }
    if (ctx->pos_timer) { lv_timer_del(ctx->pos_timer); ctx->pos_timer = NULL; }
    free(ctx);
}

/* ── Play/pause ────────────────────────────────────────────────────────── */

static void on_play_pause(lv_event_t* e) {
    lv_obj_t *label = lv_event_get_user_data(e);
    podcast_controller_toggle_play_pause(&g_podcast_app);
    if (label && lv_obj_is_valid(label)) {
        lv_label_set_text(label,
            podcast_model_is_playing(&g_podcast_app) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}
static void on_prev(lv_event_t* e) { (void)e; }
static void on_next(lv_event_t* e) { (void)e; }
/* ── 播放列表弹窗上下文 ──────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t* overlay;         // bottom sheet overlay (用于关闭)
    lv_obj_t* sel_label;       // 底部操作栏选中计数
    lv_obj_t** track_cbs;     // checkbox 对象数组
    bool*     track_checked;  // 选中状态数组
    int       episode_count;    // 队列长度
    lv_obj_t* action_bar;     // 底部操作栏, 无选中时隐藏
} PlaylistCtx;

static void playlist_set_all_checked(PlaylistCtx* ctx, bool val) {
    for (int i = 0; i < ctx->episode_count; i++) {
        ctx->track_checked[i] = val;
        if (ctx->track_cbs[i]) {
            if (val) lv_obj_add_state(ctx->track_cbs[i], LV_STATE_CHECKED);
            else     lv_obj_remove_state(ctx->track_cbs[i], LV_STATE_CHECKED);
        }
    }
}

static void playlist_update_sel_label(PlaylistCtx* ctx) {
    int n = 0;
    for (int i = 0; i < ctx->episode_count; i++) {
        if (ctx->track_checked[i]) n++;
    }

    if (ctx->sel_label) {
        lv_label_set_text_fmt(ctx->sel_label, "%d", n);
    }

    /* 操作栏显隐: 无选中时隐藏 */
    if (ctx->action_bar) {
        if (n > 0) {
            lv_obj_clear_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── 全选开关 ── */
static void playlist_on_select_all(lv_event_t* e) {
    PlaylistCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* sw = lv_event_get_current_target_obj(e);
    playlist_set_all_checked(ctx, lv_obj_has_state(sw, LV_STATE_CHECKED));
    playlist_update_sel_label(ctx);
}

/* ── 单个 checkbox 变更 ── */
static void playlist_on_checkbox_changed(lv_event_t* e) {
    lv_obj_t* cb = lv_event_get_current_target_obj(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(cb);
    PlaylistCtx* ctx = lv_event_get_user_data(e);
    ctx->track_checked[idx] = lv_obj_has_state(cb, LV_STATE_CHECKED);
    playlist_update_sel_label(ctx);
}

static void on_playlist(lv_event_t* e);

/* ── 移除选中 ── */
static void playlist_on_remove(lv_event_t* e) {
    PlaylistCtx* ctx = lv_event_get_user_data(e);
    int* old_q = g_podcast_app.model->queue;
    int  old_n = g_podcast_app.model->queue_count;
    int  old_idx = g_podcast_app.model->queue_index;

    // 统计保留数量
    int keep_n = 0;
    for (int i = 0; i < old_n; i++) {
        if (!ctx->track_checked[i]) keep_n++;
    }
    if (keep_n == 0) {
        podcast_model_set_queue(&g_podcast_app, NULL, 0);
    } else {
        int* new_q = (int*)malloc(sizeof(int) * keep_n);
        int wi = 0, new_qi = 0;
        for (int i = 0; i < old_n; i++) {
            if (!ctx->track_checked[i]) {
                new_q[wi] = old_q[i];
                if (i == old_idx) new_qi = wi;
                wi++;
            }
        }
        // 更新 queue_index: 如果当前播放项被移除，停留在下一个
        if (ctx->track_checked[old_idx]) new_qi = (new_qi < keep_n) ? new_qi : (keep_n > 0 ? keep_n - 1 : 0);
        podcast_model_set_queue(&g_podcast_app, new_q, keep_n);
        g_podcast_app.model->queue_index = new_qi;
        free(new_q);
    }

    // 关闭弹窗并重新打开刷新内容
    lv_obj_delete(ctx->overlay);
    // 重新弹出
    on_playlist(NULL);
}

/* ── 删除上下文回调 ── */
static void playlist_ctx_cleanup(lv_event_t* e) {
    PlaylistCtx* ctx = lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->track_cbs) free(ctx->track_cbs);
    if (ctx->track_checked) free(ctx->track_checked);
    free(ctx);
}

/* ── 格式化时长 ── */
static void playlist_fmt_time(int sec, char* buf, int sz) {
    snprintf(buf, sz, "%d:%02d", sec / 60, sec % 60);
}

static void on_playlist(lv_event_t* e) {
    (void)e;

    int q_count = g_podcast_app.model->queue_count;
    lv_bottom_sheet_t* bs = lv_bottom_sheet_create(NULL);
    lv_obj_t* content = lv_bottom_sheet_get_content(bs);
    lv_obj_set_flex_grow(content, 1);                     // 填满 sheet 剩余高度
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    /* ── 分配上下文 ── */
    PlaylistCtx* ctx = malloc(sizeof(PlaylistCtx));
    memset(ctx, 0, sizeof(PlaylistCtx));
    ctx->overlay = bs->overlay;
    ctx->episode_count = q_count;
    ctx->track_checked = malloc(sizeof(bool) * q_count);
    memset(ctx->track_checked, 0, sizeof(bool) * q_count);
    ctx->track_cbs = malloc(sizeof(lv_obj_t*) * q_count);
    memset(ctx->track_cbs, 0, sizeof(lv_obj_t*) * q_count);
    lv_obj_add_event_cb(bs->overlay, playlist_ctx_cleanup, LV_EVENT_DELETE, ctx);

    /* ── 列表头: 全选 switch | ALL | Title | Time ── */
    lv_obj_t* lh = lv_obj_create(content);
    lv_obj_set_size(lh, LV_PCT(100), 28);
    lv_obj_set_style_border_width(lh, 0, 0);
    lv_obj_set_style_bg_color(lh, lv_color_hex(0xFAFAFA), 0);
    lv_obj_set_style_pad_all(lh, 0, 0);

    lv_obj_t* sw = lv_switch_create(lh);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(sw, playlist_on_select_all, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t* sw_label = lv_label_create(lh);
    lv_label_set_text(sw_label, "ALL");
    lv_obj_align(sw_label, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_set_style_text_font(sw_label, g_cjk_font, 0);
    lv_obj_set_style_text_color(sw_label, lv_color_hex(0x666666), 0);

    lv_obj_t* lhdr = lv_label_create(lh);
    lv_label_set_text(lhdr, "Title");
    lv_obj_align(lhdr, LV_ALIGN_LEFT_MID, 100, 0);
    lv_obj_set_style_text_font(lhdr, g_cjk_font, 0);
    lv_obj_set_style_text_color(lhdr, lv_color_hex(0xAAAAAA), 0);

    lv_obj_t* rhdr = lv_label_create(lh);
    lv_label_set_text(rhdr, "Time  ");
    lv_obj_align(rhdr, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_font(rhdr, g_cjk_font, 0);
    lv_obj_set_style_text_color(rhdr, lv_color_hex(0xAAAAAA), 0);

    /* ── 曲目列表 (可滚动，flex_grow 占据剩余空间) ── */
    lv_obj_t* list = lv_obj_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < q_count; i++) {
        int tid = g_podcast_app.model->queue[i];
        const Episode* t = podcast_model_get_episode_by_id(&g_podcast_app, tid);
        if (!t) continue;

        /* row */
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), 32);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        /* 当前播放项高亮 */
        if (i == g_podcast_app.model->queue_index) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0xE3F2FD), 0);
        }

        /* checkbox */
        lv_obj_t* cb = lv_checkbox_create(row);
        lv_obj_set_width(cb, 24);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_pad_all(cb, 0, 0);
        lv_obj_set_user_data(cb, (void*)(uintptr_t)i);
        lv_obj_add_event_cb(cb, playlist_on_checkbox_changed, LV_EVENT_VALUE_CHANGED, ctx);
        ctx->track_cbs[i] = cb;

        /* 序号 */
        char num[12];
        snprintf(num, sizeof(num), "%02d", i + 1);
        lv_obj_t* n = lv_label_create(row);
        lv_label_set_text(n, num);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 32, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(n, g_cjk_font, 0);

        /* 标题 */
        lv_obj_t* title = lv_label_create(row);
        lv_label_set_text(title, t->title);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 56, 0);
        lv_obj_set_style_text_font(title, g_cjk_font, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

        /* 时长 */
        char ds[16]; playlist_fmt_time(t->duration_sec, ds, sizeof(ds));
        lv_obj_t* d = lv_label_create(row);
        lv_label_set_text(d, ds);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_text_color(d, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(d, g_cjk_font, 0);

        /* 底部分隔线 */
        lv_obj_t* line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
    }

    /* ── 底部操作栏 ── */
    lv_obj_t* bar4 = lv_obj_create(content);
    lv_obj_set_size(bar4, LV_PCT(100), 36);
    lv_obj_set_style_border_width(bar4, 0, 0);
    lv_obj_set_style_bg_color(bar4, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(bar4, 4, 0);
    lv_obj_set_flex_flow(bar4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar4, 8, 0);
    lv_obj_set_scrollbar_mode(bar4, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(bar4, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏, 选中后显示 */
    ctx->action_bar = bar4;

    ctx->sel_label = lv_label_create(bar4);
    lv_obj_set_width(ctx->sel_label, 30);
    lv_label_set_text(ctx->sel_label, "0");
    lv_obj_set_style_text_align(ctx->sel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ctx->sel_label, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_text_font(ctx->sel_label, g_cjk_font, 0);

    lv_obj_t* sp = lv_obj_create(bar4);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t* rm = lv_button_create(bar4);
    lv_obj_set_size(rm, 80, 28);
    lv_obj_set_style_bg_color(rm, lv_color_hex(0xF44336), 0);
    lv_obj_t* rml = lv_label_create(rm);
    lv_label_set_text(rml, "Remove");
    lv_obj_center(rml);
    lv_obj_set_style_text_color(rml, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(rm, playlist_on_remove, LV_EVENT_CLICKED, ctx);

    lv_bottom_sheet_set_height(bs, LV_PCT(80));
}

// ---- 定时关机 radio 互斥上下文 ----
typedef struct {
    lv_obj_t* cb_time;
    lv_obj_t* cb_tracks;
    lv_obj_t* inp_time;
    lv_obj_t* inp_tracks;
} timer_radio_ctx_t;

static void _set_input_enabled(lv_obj_t* inp, bool enabled) {
    if (!inp) return;
    if (enabled) {
        lv_obj_remove_state(inp, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(inp, LV_STATE_DISABLED);
    }
}

static void on_timer_radio_cb(lv_event_t* e) {
    lv_obj_t* cb = lv_event_get_target(e);
    timer_radio_ctx_t* ctx = lv_event_get_user_data(e);

    if (cb == ctx->cb_time) {
        lv_obj_add_state(ctx->cb_time, LV_STATE_CHECKED);
        lv_obj_remove_state(ctx->cb_tracks, LV_STATE_CHECKED);
        _set_input_enabled(ctx->inp_time, true);
        _set_input_enabled(ctx->inp_tracks, false);
    } else {
        lv_obj_add_state(ctx->cb_tracks, LV_STATE_CHECKED);
        lv_obj_remove_state(ctx->cb_time, LV_STATE_CHECKED);
        _set_input_enabled(ctx->inp_tracks, true);
        _set_input_enabled(ctx->inp_time, false);
    }
}

static void delete_radio_ctx_cb(lv_event_t* e) {
    timer_radio_ctx_t* ctx = lv_event_get_user_data(e);
    if (ctx) free(ctx);
}

static void on_timer_confirm(lv_event_t* e) {
    lv_bottom_sheet_t* bs = lv_event_get_user_data(e);
    printf("[INF] Sleep timer confirmed\n"); fflush(stdout);
    lv_bottom_sheet_close(bs);
}

static void on_timer(lv_event_t* e) {
    (void)e;
    lv_bottom_sheet_t* bs = lv_bottom_sheet_create(NULL);
    lv_obj_t* content = lv_bottom_sheet_get_content(bs);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_all(content, 16, 0);

    timer_radio_ctx_t* radio_ctx = malloc(sizeof(timer_radio_ctx_t));
    memset(radio_ctx, 0, sizeof(timer_radio_ctx_t));

    // ---- Stop by time (默认选中) ----
    lv_obj_t* row_time = lv_obj_create(content);
    lv_obj_remove_style_all(row_time);
    lv_obj_set_size(row_time, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row_time, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(row_time, 0, 0);

    radio_ctx->cb_time = lv_checkbox_create(row_time);
    lv_checkbox_set_text(radio_ctx->cb_time, "Stop by time");
    lv_obj_add_state(radio_ctx->cb_time, LV_STATE_CHECKED);

    radio_ctx->inp_time = lv_number_input_create(row_time, NULL, 15, 5, "minutes");
    lv_num_input_set_height(radio_ctx->inp_time, 32);
    lv_obj_set_style_margin_top(radio_ctx->inp_time, 6, 0);
    lv_obj_set_style_pad_left(radio_ctx->inp_time, 16, 0);

    // ---- Stop by tracks ----
    lv_obj_t* row_tracks = lv_obj_create(content);
    lv_obj_remove_style_all(row_tracks);
    lv_obj_set_size(row_tracks, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row_tracks, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(row_tracks, 0, 0);

    radio_ctx->cb_tracks = lv_checkbox_create(row_tracks);
    lv_checkbox_set_text(radio_ctx->cb_tracks, "Stop by tracks");

    radio_ctx->inp_tracks = lv_number_input_create(row_tracks, NULL, 1, 1, "tracks");
	lv_num_input_set_height(radio_ctx->inp_tracks, 32);
    lv_obj_set_style_margin_top(radio_ctx->inp_tracks, 6, 0);
    lv_obj_set_style_pad_left(radio_ctx->inp_tracks, 16, 0);
    _set_input_enabled(radio_ctx->inp_tracks, false);

    lv_obj_add_event_cb(radio_ctx->cb_time,   on_timer_radio_cb, LV_EVENT_CLICKED, radio_ctx);
    lv_obj_add_event_cb(radio_ctx->cb_tracks, on_timer_radio_cb, LV_EVENT_CLICKED, radio_ctx);
    lv_obj_add_event_cb(bs->overlay, delete_radio_ctx_cb, LV_EVENT_DELETE, radio_ctx);

    lv_obj_t* btn = lv_button_create(content);
    lv_obj_set_size(btn, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1976D2), 0);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "Confirm");
    lv_obj_center(bl);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(btn, on_timer_confirm, LV_EVENT_CLICKED, bs);

    /* height: auto (LV_SIZE_CONTENT by default) */
}

static void fmt_time(int sec, char* buf, int sz) {
    snprintf(buf, sz, "%d:%02d", sec / 60, sec % 60);
}

static lv_obj_t* build_player_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;

    /* Determine whether we came from a channel episode (have nav_ctx) or
     * from the tab bar (no nav_ctx).  Only show the back button when
     * navigating from channel → player so the user can return to channel. */
    bool from_channel = (app->view->page_nav.nav_ctx != NULL);

    int eid = 0;
    if (app->view->page_nav.nav_ctx) {
        eid = *(int*)app->view->page_nav.nav_ctx;
        free(app->view->page_nav.nav_ctx);
        app->view->page_nav.nav_ctx = NULL;
        /* New episode selected — start playback */
        if (eid > 0) podcast_controller_play_episode(app, eid);
    }
    /* Fallback: no nav_ctx means we're coming from tab bar — show what's playing */
    if (eid == 0) eid = podcast_model_get_current_episode_id(app);

    const Episode* ep = podcast_model_get_episode_by_id(app, eid);
    const Channel* ch = ep ? podcast_model_get_channel_by_id(app, ep->channel_id) : NULL;
    const char* tname = ep ? ep->title : "Nothing playing";
    const char* tartist = ch ? ch->artist : "";
    int duration = ep ? ep->duration_sec : 0;

    Page page = lv_page_create(NULL, from_channel, page_navigator_navigate_back, &app->view->page_nav);
    int container_h = lv_display_get_vertical_resolution(lv_display_get_default())
                      - LV_STATUS_BAR_HEIGHT - LV_PAGE_HEADER_HEIGHT - TAB_BAR_HEIGHT;
    lv_obj_set_height(page.container, container_h);

    // 内容区
    lv_obj_t* main = page.container;
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(main, LV_DIR_NONE);

    // 封面 60x60
    lv_obj_t* cover = lv_obj_create(main);
    lv_obj_set_size(cover, 60, 60);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 8, 0);
    lv_obj_t* ci = lv_label_create(cover);
    lv_label_set_text(ci, LV_SYMBOL_AUDIO);
    lv_obj_center(ci);
    lv_obj_set_style_text_color(ci, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ci, g_cjk_font, 0);

    // 歌名
    lv_obj_t* tn = lv_label_create(main);
    lv_label_set_text(tn, tname);
    lv_obj_set_style_text_font(tn, g_cjk_font, 0);
    lv_obj_set_style_pad_top(tn, 8, 0);

    // 歌手
    lv_obj_t* ta = lv_label_create(main);
    lv_label_set_text(ta, tartist);
    lv_obj_set_style_text_color(ta, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ta, g_cjk_font, 0);

    /* ── PlayerCtx: position polling ────────────────────────────────── */
    PlayerCtx *pctx = (PlayerCtx *)calloc(1, sizeof(PlayerCtx));
    pctx->duration  = duration;
    pctx->episode_id = eid;
    pctx->channel_id = ep ? ep->channel_id : 0;

    /* Restore saved position if resuming */
    int saved_pos = cache_playback_get_position(eid);
    if (saved_pos > 0 && saved_pos < duration) {
        /* Resume from saved position — just show it on the bar.
         * Actual audio seek would need audio_player support. */
    }

    // 进度条
    lv_obj_t* pbar = lv_bar_create(main);
    lv_obj_set_size(pbar, LV_PCT(85), 4);
    lv_obj_set_style_pad_top(pbar, 12, 0);
    lv_bar_set_range(pbar, 0, duration > 0 ? duration : 1);
    lv_bar_set_value(pbar, saved_pos, LV_ANIM_OFF);
    pctx->pbar = pbar;

    // 时间
    lv_obj_t* ptime = lv_obj_create(main);
    lv_obj_set_size(ptime, LV_PCT(85), 18);
    lv_obj_set_style_border_width(ptime, 0, 0);
    lv_obj_set_style_bg_opa(ptime, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ptime, 0, 0);

    char tbuf[16]; fmt_time(saved_pos, tbuf, sizeof(tbuf));
    lv_obj_t* ct = lv_label_create(ptime);
    lv_label_set_text(ct, tbuf);
    lv_obj_align(ct, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(ct, g_cjk_font, 0);
    pctx->time_label = ct;

    char dbuf[16]; fmt_time(duration, dbuf, sizeof(dbuf));
    lv_obj_t* dt = lv_label_create(ptime);
    lv_label_set_text(dt, dbuf);
    lv_obj_align(dt, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(dt, g_cjk_font, 0);

    /* Position poll timer — 500ms */
    pctx->pos_timer = lv_timer_create(player_pos_timer_cb, 500, pctx);

    /* Cleanup on page/screen delete */
    lv_obj_add_event_cb(page.screen, player_ctx_cleanup, LV_EVENT_DELETE, pctx);

    // 控制按钮 (5个)
    lv_obj_t* row = lv_obj_create(main);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    struct { const char* sym; void (*cb)(lv_event_t*); void* ud; } btns[] = {
        {LV_SYMBOL_LIST,  on_playlist, NULL},
        {LV_SYMBOL_PREV,  on_prev,     NULL},
        {LV_SYMBOL_PLAY,  on_play_pause,NULL},
        {LV_SYMBOL_NEXT,  on_next,     NULL},
        {LV_SYMBOL_STOP,  on_timer,    NULL},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t* b = lv_button_create(row);
        lv_obj_set_size(b, 36, 36);
        lv_obj_set_style_radius(b, 18, 0);
        if (i == 2) lv_obj_set_style_bg_color(b, lv_color_hex(0x1976D2), 0);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, btns[i].sym);
        lv_obj_center(l);
        if (i == 2) lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        if (i == 2) pctx->pp_label = l;
        void* ud = (i == 2) ? (void*)l : btns[i].ud;
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, ud);
    }

    podcast_view_create_bottom_tab_bar(page.screen, TAB_PLAYER);
    printf("[INF] Player page built\n"); fflush(stdout);
    return page.screen;
}

void podcast_view_player_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_PLAYER, build_player_page);
}
