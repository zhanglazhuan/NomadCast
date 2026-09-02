/**
 * @file view_album.c
 * @brief Channel detail page — async track loading via RSS feed
 *
 * Shows album info immediately, polls for tracks loaded from backend RSS parse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "view_channel.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lv_bottom_sheet.h"
#include "lv_toast.h"

extern PodcastApp g_podcast_app;
extern const lv_image_dsc_t ic_info;

/* Stable episode id = channel_id * this + global episode index.
 * Must exceed the max episodes per channel (100+ pages → 1000+ episodes). */
#define EPISODE_ID_PER_CHANNEL 100000

typedef struct {
    lv_obj_t  *sel_label;
    lv_obj_t **track_cbs;
    bool      *track_checked;
    int        episode_count;
    lv_obj_t  *action_bar;
    lv_obj_t  *list_container;
    lv_obj_t  *loading_label;
    lv_timer_t *poll_timer;
    int        channel_id;
    int        cur_page;         /* 0-indexed current page */
    int        total_pages;
    lv_obj_t  *page_label;      /* "Page 1/3" label in header */
    lv_obj_t  *prev_btn;
    lv_obj_t  *next_btn;
} ChannelPageCtx;

static void format_duration(int sec, char *buf, int sz) {
    snprintf(buf, sz, "%d:%02d", sec / 60, sec % 60);
}

static void set_all_checked(ChannelPageCtx *ctx, bool val) {
    for (int i = 0; i < ctx->episode_count; i++) {
        ctx->track_checked[i] = val;
        if (ctx->track_cbs[i]) {
            if (val) lv_obj_add_state(ctx->track_cbs[i], LV_STATE_CHECKED);
            else     lv_obj_remove_state(ctx->track_cbs[i], LV_STATE_CHECKED);
        }
    }
}

static void update_sel_label(ChannelPageCtx *ctx) {
    int n = 0;
    for (int i = 0; i < ctx->episode_count; i++)
        if (ctx->track_checked[i]) n++;
    if (ctx->sel_label) lv_label_set_text_fmt(ctx->sel_label, "%d", n);
    if (ctx->action_bar) {
        if (n > 0) lv_obj_clear_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_select_all_switch(lv_event_t *e) {
    ChannelPageCtx *ctx = lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_current_target_obj(e);
    set_all_checked(ctx, lv_obj_has_state(sw, LV_STATE_CHECKED));
    update_sel_label(ctx);
}

static void on_checkbox_changed(lv_event_t *e) {
    lv_obj_t *cb = lv_event_get_current_target_obj(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(cb);
    ChannelPageCtx *ctx = lv_event_get_user_data(e);
    ctx->track_checked[idx] = lv_obj_has_state(cb, LV_STATE_CHECKED);
    update_sel_label(ctx);
}

static void on_track_clicked(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    int track_id = (int)(uintptr_t)lv_obj_get_user_data(row);

    /* Remote M4A can't stream (server transcode is disabled) — it must be
     * downloaded first. Stay on this page and toast instead of jumping to
     * the player page. */
    if (!podcast_controller_episode_playable(&g_podcast_app, track_id)) {
        lv_toast_show("M4A 需下载后播放", 2000);
        return;
    }

    int *id_ptr = malloc(sizeof(int));
    *id_ptr = track_id;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_CHANNEL, PAGE_PLAYER, id_ptr);
}

static lv_obj_t *create_track_row(lv_obj_t *parent, const Episode *track, int index, int episode_num, ChannelPageCtx *ctx) {
    lv_obj_t *row = lv_obj_create(parent);
    if (!row) return NULL;  /* heap exhausted */
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_clip_corner(row, true, 0);
    lv_obj_set_user_data(row, (void *)(uintptr_t)track->id);
    lv_obj_add_event_cb(row, on_track_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cb = lv_checkbox_create(row);
    lv_obj_set_width(cb, 24);
    lv_obj_align(cb, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_pad_all(cb, 0, 0);
    lv_obj_set_user_data(cb, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(cb, on_checkbox_changed, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->track_cbs[index] = cb;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d", episode_num);
    lv_obj_t *n = lv_label_create(row);
    if (!n) return row;  /* heap exhausted — bail gracefully */
    lv_label_set_text(n, buf);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 32, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(0x999999), 0);

    /* Title — 2 lines max, overflow clipped by row's clip_corner.
     * Available width: screen 240 - pad 8×2 - checkbox 24 - gaps to left(56) - right duration ~48 */
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, track->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(t, g_cjk_font, 0);
    lv_obj_set_width(t, LV_PCT(57));  /* ~128 px on 240-wide screen */
    lv_obj_set_style_max_height(t, 36, 0);  /* 2 × line_height (18px) */
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 56, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x333333), 0);

    char ds[16]; format_duration(track->duration_sec, ds, sizeof(ds));
    lv_obj_t *d = lv_label_create(row);
    lv_label_set_text(d, ds);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(0x999999), 0);

    lv_obj_t *line = lv_obj_create(row);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    return row;
}

static void render_page(ChannelPageCtx *ctx);
static void show_loading_and_poll(ChannelPageCtx *ctx);
static void page_poll_cb(lv_timer_t *timer);

static void on_prev_page(lv_event_t *e) {
    ChannelPageCtx *ctx = (ChannelPageCtx *)lv_event_get_user_data(e);
    if (ctx && ctx->cur_page > 0) {
        ctx->cur_page--;
        controller_rss_reparse(ctx->cur_page * EPISODES_PER_PAGE, EPISODES_PER_PAGE);
        show_loading_and_poll(ctx);
    }
}
static void on_next_page(lv_event_t *e) {
    ChannelPageCtx *ctx = (ChannelPageCtx *)lv_event_get_user_data(e);
    if (ctx && ctx->cur_page < ctx->total_pages - 1) {
        ctx->cur_page++;
        controller_rss_reparse(ctx->cur_page * EPISODES_PER_PAGE, EPISODES_PER_PAGE);
        show_loading_and_poll(ctx);
    }
}

/* Import the freshly-fetched page (g_rss_result) into the model and build the
 * visible rows. The model holds ONLY this page's episodes, so per-page RAM stays
 * constant regardless of how many pages (100+) the feed has. */
static void render_page(ChannelPageCtx *ctx) {
    if (!ctx || !lv_obj_is_valid(ctx->list_container)) return;
    PodcastApp *app = &g_podcast_app;
    lv_obj_t *list = ctx->list_container;
    rss_feed_t *rss = g_rss_result();

    if (!rss || rss->episode_count <= 0) return;

    /* Clear old rows + per-page arrays. */
    free(ctx->track_checked); ctx->track_checked = NULL;
    free(ctx->track_cbs);     ctx->track_cbs = NULL;
    ctx->episode_count = 0;
    lv_obj_clean(list);

    int n = rss->episode_count;
    int start = ctx->cur_page * EPISODES_PER_PAGE;

    /* Import this page with stable ids (channel_id * base + global index). */
    Episode *tracks = (Episode *)heap_caps_calloc(n, sizeof(Episode), MALLOC_CAP_SPIRAM);
    if (!tracks) { printf("[ERR] calloc tracks failed\n"); return; }
    for (int i = 0; i < n; i++) {
        Episode *t = &tracks[i];
        rss_episode_t *re = &rss->episodes[i];
        t->id         = ctx->channel_id * EPISODE_ID_PER_CHANNEL + (start + i);
        t->channel_id = ctx->channel_id;
        strncpy(t->title, re->title, sizeof(t->title) - 1);
        strncpy(t->audio_url, re->audio_url, sizeof(t->audio_url) - 1);
        strncpy(t->pub_date, re->pub_date, sizeof(t->pub_date) - 1);
        if (re->duration[0]) {
            char *colon = strchr(re->duration, ':');
            t->duration_sec = colon ? atoi(re->duration) * 60 + atoi(colon + 1) : atoi(re->duration);
        }
    }
    podcast_model_set_current_channel(app,
        podcast_model_get_channel_by_id(app, ctx->channel_id),
        tracks, n);
    app->model->current_channel_total_episodes = rss->total_episodes;

    /* Pagination bookkeeping. */
    int total = rss->total_episodes;
    ctx->total_pages = (total + EPISODES_PER_PAGE - 1) / EPISODES_PER_PAGE;
    if (ctx->total_pages < 1) ctx->total_pages = 1;
    if (ctx->cur_page >= ctx->total_pages) ctx->cur_page = ctx->total_pages - 1;
    if (ctx->cur_page < 0) ctx->cur_page = 0;

    /* Build rows — the model holds only this page, so index is 0..n-1. */
    ctx->track_checked = (bool *)calloc(n, sizeof(bool));
    ctx->track_cbs     = (lv_obj_t **)calloc(n, sizeof(lv_obj_t *));
    for (int i = 0; i < n; i++) {
        const Episode *ep = podcast_model_get_current_channel_episode(app, i);
        if (ep) create_track_row(list, ep, i, start + i + 1, ctx);
    }
    ctx->episode_count = n;

    /* Page label + prev/next state. */
    if (ctx->page_label)
        lv_label_set_text_fmt(ctx->page_label, "%d/%d", ctx->cur_page + 1, ctx->total_pages);
    if (ctx->prev_btn)
        lv_obj_set_style_bg_color(ctx->prev_btn,
            ctx->cur_page > 0 ? lv_color_hex(0x1976D2) : lv_color_hex(0xCCCCCC), 0);
    if (ctx->next_btn)
        lv_obj_set_style_bg_color(ctx->next_btn,
            ctx->cur_page < ctx->total_pages - 1 ? lv_color_hex(0x1976D2) : lv_color_hex(0xCCCCCC), 0);

    if (ctx->sel_label) lv_label_set_text(ctx->sel_label, "0");
    if (ctx->action_bar) lv_obj_add_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
}

/* Clear the list, show "Loading...", and arm the poll timer that renders the
 * page once the async RSS fetch (already triggered by the caller) completes. */
static void show_loading_and_poll(ChannelPageCtx *ctx) {
    if (!ctx || !lv_obj_is_valid(ctx->list_container)) return;

    free(ctx->track_checked); ctx->track_checked = NULL;
    free(ctx->track_cbs);     ctx->track_cbs = NULL;
    ctx->episode_count = 0;
    lv_obj_clean(ctx->list_container);

    ctx->loading_label = lv_label_create(ctx->list_container);
    lv_label_set_text(ctx->loading_label, "Loading...");
    lv_obj_center(ctx->loading_label);
    lv_obj_set_style_text_color(ctx->loading_label, lv_color_hex(0x999999), 0);

    if (ctx->poll_timer) lv_timer_del(ctx->poll_timer);
    ctx->poll_timer = lv_timer_create(page_poll_cb, 300, ctx);
}

static void page_poll_cb(lv_timer_t *timer) {
    ChannelPageCtx *ctx = (ChannelPageCtx *)lv_timer_get_user_data(timer);
    if (!ctx || !lv_obj_is_valid(ctx->list_container)) {
        lv_timer_del(timer); ctx->poll_timer = NULL; return;
    }

    if (!g_rss_done()) return;
    if (g_rss_channel_id() != ctx->channel_id) return;

    lv_timer_del(timer); ctx->poll_timer = NULL;
    if (ctx->loading_label) { lv_obj_del(ctx->loading_label); ctx->loading_label = NULL; }

    if (g_rss_ok() && g_rss_result() && g_rss_result()->episode_count > 0) {
        render_page(ctx);
        printf("[INF] Channel: page %d, %d episodes\n",
               ctx->cur_page + 1, g_rss_result()->episode_count); fflush(stdout);
    } else {
        lv_obj_t *empty = lv_label_create(ctx->list_container);
        const char *err = (g_rss_result() && g_rss_result()->error[0])
                          ? g_rss_result()->error
                          : "Failed to load episodes.\nCheck network connection.";
        lv_label_set_text(empty, err);
        lv_obj_center(empty);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void on_download_clicked(lv_event_t *e) {
    ChannelPageCtx *ctx = (ChannelPageCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    int cid = ctx->channel_id;
    const Channel *ch = podcast_model_get_channel_by_id(&g_podcast_app, cid);
    const char *ch_name = ch ? ch->title : "Unknown";
    int submitted = 0;
    for (int i = 0; i < ctx->episode_count; i++) {
        if (!ctx->track_checked[i]) continue;
        lv_obj_t *row = lv_obj_get_parent(ctx->track_cbs[i]);
        int eid = (int)(uintptr_t)lv_obj_get_user_data(row);
        const Episode *ep = podcast_model_get_episode_by_id(&g_podcast_app, eid);
        if (ep && ep->audio_url[0]) {
            podcast_controller_download_episode_ex2(&g_podcast_app, ep->audio_url, ep->title, ch_name, cid, eid, ep->duration_sec, ch->collection_id);
            submitted++;
        }
    }
    if (submitted > 0)
        lv_toast_show("Download queued", 2000);
}

/* Play selected episodes: collect checked episodes, build a queue, and navigate
 * to the player page (where the playlist bottom sheet shows them). */
static void on_play_selected_clicked(lv_event_t *e) {
    ChannelPageCtx *ctx = (ChannelPageCtx *)lv_event_get_user_data(e);
    if (!ctx || ctx->episode_count <= 0) return;

    /* Count checked */
    int n = 0;
    for (int i = 0; i < ctx->episode_count; i++)
        if (ctx->track_checked[i]) n++;
    if (n == 0) return;

    int *ids = (int *)malloc(sizeof(int) * n);
    if (!ids) return;
    int wi = 0;
    for (int i = 0; i < ctx->episode_count; i++) {
        if (!ctx->track_checked[i]) continue;
        lv_obj_t *row = lv_obj_get_parent(ctx->track_cbs[i]);
        ids[wi++] = (int)(uintptr_t)lv_obj_get_user_data(row);
    }
    podcast_model_set_queue(&g_podcast_app, ids, n);
    int *first_id = malloc(sizeof(int));
    *first_id = ids[0];
    free(ids);
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_CHANNEL, PAGE_PLAYER, first_id);
}

static void on_sheet_delete(lv_event_t *e) {
    /* lv_bottom_sheet's own delete_event_cb already frees the struct.
     * We only need this callback if we had context to clear. */
    (void)e;
}

static void on_info_clicked(lv_event_t *e) {
    ChannelPageCtx *ctx = lv_event_get_user_data(e);
    const Channel *channel = podcast_model_get_channel_by_id(&g_podcast_app, ctx->channel_id);
    if (!channel) return;

    lv_obj_t *scr = lv_screen_active();
    lv_bottom_sheet_t *bs = lv_bottom_sheet_create(scr);
    lv_obj_add_event_cb(bs->overlay, on_sheet_delete, LV_EVENT_DELETE, bs);

    lv_obj_t *content = lv_bottom_sheet_get_content(bs);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_set_style_pad_all(content, 16, 0);

    /* Artist */
    lv_obj_t *artist_label = lv_label_create(content);
    lv_label_set_text(artist_label, channel->artist);
    lv_obj_set_style_text_font(artist_label, g_cjk_font, 0);
    lv_obj_set_style_text_color(artist_label, lv_color_hex(0x333333), 0);

    /* Update time */
    if (channel->upload_time[0]) {
        lv_obj_t *time_label = lv_label_create(content);
        lv_label_set_text(time_label, channel->upload_time);
        lv_obj_set_style_text_font(time_label, g_cjk_font, 0);
        lv_obj_set_style_text_color(time_label, lv_color_hex(0x999999), 0);
    }

    /* Description — full text, wrapped */
    if (channel->description[0]) {
        lv_obj_t *desc_label = lv_label_create(content);
        lv_label_set_text(desc_label, channel->description);
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc_label, LV_PCT(100));
        lv_obj_set_style_text_font(desc_label, g_cjk_font, 0);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(0x666666), 0);
    }
}

/* ── Bottom action bar (选中数 | spacer | Download | Play) ──
 * 浮动吸底图层：挂在 page.screen 上、FLOATING 浮动、吸底对齐，浮在列表之上。
 * (与 view_download_task.c 的 build_action_bar 结构保持一致。) */
static lv_obj_t *build_action_bar(lv_obj_t *parent, ChannelPageCtx *ctx, int channel_id) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(bar, 20, 0);
    lv_obj_set_style_shadow_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    ctx->action_bar = bar;

    /* 选中计数 */
    ctx->sel_label = lv_label_create(bar);
    lv_obj_set_width(ctx->sel_label, 30);
    lv_label_set_text(ctx->sel_label, "0");
    lv_obj_set_style_text_align(ctx->sel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ctx->sel_label, lv_color_hex(0x1976D2), 0);

    /* 弹性空白 */
    lv_obj_t *sp = lv_obj_create(bar);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    /* Download 按钮 */
    lv_obj_t *da = lv_button_create(bar);
    lv_obj_set_size(da, 80, 28);
    lv_obj_set_style_bg_color(da, lv_color_hex(0x4CAF50), 0);
    lv_obj_t *dal = lv_label_create(da);
    lv_label_set_text(dal, "Download");
    lv_obj_center(dal);
    lv_obj_set_style_text_color(dal, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(da, on_download_clicked, LV_EVENT_CLICKED, ctx);

    /* Play 按钮 */
    lv_obj_t *pa = lv_button_create(bar);
    lv_obj_set_size(pa, 80, 28);
    lv_obj_set_style_bg_color(pa, lv_color_hex(0x1976D2), 0);
    lv_obj_t *pal = lv_label_create(pa);
    lv_label_set_text(pal, "Play");
    lv_obj_center(pal);
    lv_obj_set_style_text_color(pal, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(pa, on_play_selected_clicked, LV_EVENT_CLICKED, ctx);

    return bar;
}

static lv_obj_t *build_channel_page(struct PodcastApp *app, void *user_data) {
    (void)user_data;

    int channel_id = 0;
    if (app->view->page_nav.nav_ctx) {
        channel_id = *(int *)app->view->page_nav.nav_ctx;
        free(app->view->page_nav.nav_ctx);
        app->view->page_nav.nav_ctx = NULL;
    } else {
        /* Popping back from player/sub-page — restore from current channel.
         * Local channel pages do not populate current_channel, so recover the
         * owning channel from the episode that was just selected instead. */
        const Channel *ch = podcast_model_get_current_channel(app);
        if (ch) {
            channel_id = ch->id;
        } else {
            int episode_id = podcast_model_get_current_episode_id(app);
            const Episode *episode = podcast_model_get_episode_by_id(app, episode_id);
            if (episode) channel_id = episode->channel_id;
        }
    }

    const Channel *channel = podcast_model_get_channel_by_id(app, channel_id);

    /* Network channel → fetch RSS */
    if (channel && !channel->downloaded)
        podcast_controller_fetch_channel_episodes(app, channel_id);
    if (!channel) {
        Page page = lv_page_create(NULL, true, page_navigator_navigate_back, &app->view->page_nav);
        lv_obj_t *l = lv_label_create(page.container);
        lv_label_set_text(l, "Channel not found"); lv_obj_center(l);
        return page.screen;
    }

    Page page = lv_page_create(channel->title, true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_set_style_bg_color(page.screen, lv_color_hex(0xF0F0F0), 0);

    ChannelPageCtx *ctx = (ChannelPageCtx *)calloc(1, sizeof(ChannelPageCtx));
    ctx->channel_id = channel_id;
    ctx->cur_page = 0;
    ctx->total_pages = 1;
    app->view->page_nav.nav_ctx = ctx;

    /* Info button — shows channel details in bottom sheet */
    if (page.header_right) {
        lv_obj_t *info_btn = lv_button_create(page.header_right);
        lv_obj_set_size(info_btn, 24, 24);
        lv_obj_set_style_bg_opa(info_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info_btn, 0, 0);
        lv_obj_set_style_shadow_width(info_btn, 0, 0);
        lv_obj_set_style_pad_all(info_btn, 0, 0);
        lv_obj_add_event_cb(info_btn, on_info_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_t *info_img = lv_image_create(info_btn);
        lv_image_set_src(info_img, &ic_info);
        lv_obj_center(info_img);
        lv_obj_set_style_img_recolor(info_img, lv_color_hex(0x666666), 0);
        lv_obj_set_style_img_recolor_opa(info_img, LV_OPA_COVER, 0);
    }

    /* ── Episode list header ── */
    lv_obj_t *lh = lv_obj_create(page.container);
    lv_obj_set_size(lh, LV_PCT(100), 28);
    lv_obj_set_style_border_width(lh, 0, 0);
    lv_obj_set_style_bg_color(lh, lv_color_hex(0xFAFAFA), 0);
    lv_obj_set_style_pad_all(lh, 0, 0);

    lv_obj_t *sw = lv_switch_create(lh);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(sw, on_select_all_switch, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t *sw_label = lv_label_create(lh);
    lv_label_set_text(sw_label, "ALL");
    lv_obj_align(sw_label, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_set_style_text_font(sw_label, g_cjk_font, 0);
    lv_obj_set_style_text_color(sw_label, lv_color_hex(0x666666), 0);

    /* Page nav: < X/N > */
    ctx->prev_btn = lv_button_create(lh);
    lv_obj_set_size(ctx->prev_btn, 28, 22);
    lv_obj_align(ctx->prev_btn, LV_ALIGN_RIGHT_MID, -110, 0);
    lv_obj_set_style_pad_all(ctx->prev_btn, 0, 0);
    lv_obj_set_style_bg_opa(ctx->prev_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(ctx->prev_btn, 0, 0);
    lv_obj_t *pt = lv_label_create(ctx->prev_btn);
    lv_label_set_text(pt, "<"); lv_obj_center(pt);
    lv_obj_set_style_text_color(pt, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_text_font(pt, g_cjk_font, 0);

    ctx->page_label = lv_label_create(lh);
    lv_label_set_text(ctx->page_label, "1/1");
    lv_obj_align(ctx->page_label, LV_ALIGN_RIGHT_MID, -78, 0);
    lv_obj_set_style_text_color(ctx->page_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(ctx->page_label, g_cjk_font, 0);

    ctx->next_btn = lv_button_create(lh);
    lv_obj_set_size(ctx->next_btn, 28, 22);
    lv_obj_align(ctx->next_btn, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_set_style_pad_all(ctx->next_btn, 0, 0);
    lv_obj_set_style_bg_opa(ctx->next_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->next_btn, 0, 0);
    lv_obj_set_style_shadow_width(ctx->next_btn, 0, 0);
    lv_obj_t *nt = lv_label_create(ctx->next_btn);
    lv_label_set_text(nt, ">"); lv_obj_center(nt);
    lv_obj_set_style_text_color(nt, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_text_font(nt, g_cjk_font, 0);
    lv_obj_add_event_cb(ctx->prev_btn, on_prev_page, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(ctx->next_btn, on_next_page, LV_EVENT_CLICKED, ctx);

    /* ── Episode list container ── */
    lv_obj_t *list = lv_obj_create(page.container);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    ctx->list_container = list;

    /* ── Bottom action bar (floating over list) ── */
    build_action_bar(page.screen, ctx, channel_id);

    /* ── Build episode list ── */
    if (channel && channel->downloaded) {
        int local_count = 0;
        for (int i = 0; i < app->model->local_episode_count; i++)
            if (app->model->local_episodes[i].channel_id == channel_id) local_count++;
        ctx->episode_count = local_count;
        if (local_count > 0) {
            ctx->track_checked = (bool *)calloc(local_count, sizeof(bool));
            ctx->track_cbs = (lv_obj_t **)calloc(local_count, sizeof(lv_obj_t *));
            int row_idx = 0;
            for (int i = 0; i < app->model->local_episode_count; i++) {
                if (app->model->local_episodes[i].channel_id == channel_id) {
                    create_track_row(list, &app->model->local_episodes[i], row_idx, row_idx + 1, ctx);
                    row_idx++;
                }
            }
        }
    } else {
        /* Network channel — async fetch page 0 (already triggered above), then
         * show loading and poll for the result. */
        ctx->cur_page = 0;
        ctx->total_pages = 1;
        show_loading_and_poll(ctx);
    }

    printf("[INF] Channel page: %s\n", channel->title); fflush(stdout);
    return page.screen;
}

void podcast_view_album_init_registry(struct PodcastApp *app) {
    PAGE_REGISTE(app, PAGE_CHANNEL, build_channel_page);
}
