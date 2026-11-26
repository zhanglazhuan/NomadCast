/**
 * @file view_recently_played.c
 * @brief 最近播放页 — 展示本地播放历史(最新在上),点击直接播放
 *
 * 数据源是 system/audio_player/play_history 模块:按 path 记录的最近 N 个
 * 本地播放过的文件 + 续播字节偏移。本页只读、只展示,播放时直接调用
 * hal_audio_play_file(path),audio_player_play() 内部会命中 play_history 并
 * 自动 seek 到上次位置(因此"点击记录"即续播)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "view_recently_played.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../hal.h"
#include "lv_page.h"
#include "lang.h"
#include "view_player.h"

#ifndef _WIN32
#include "play_history.h"
#endif

extern PodcastApp g_podcast_app;

/* 行点击 → 进入播放页,以"裸路径"模式续播该本地文件。idx 为 play_history_get()
 * 的索引,通过 user_data 传递,避免为每个 path 额外 malloc 一份拷贝。 */
#ifndef _WIN32
static void on_row_clicked(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(row);
    play_history_entry_t entry;
    if (!play_history_get(idx, &entry) || !entry.path[0]) return;

    player_open_arg_t *arg = malloc(sizeof(player_open_arg_t));
    if (!arg) return;
    memset(arg, 0, sizeof(*arg));
    snprintf(arg->path, sizeof(arg->path), "%s", entry.path);
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_RECENTLY_PLAYED, PAGE_PLAYER, arg);
}
#endif /* !_WIN32 */

/* 从完整路径取出可读标题:basename 去掉扩展名。 */
static void path_to_title(const char *path, char *out, int out_size) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, out_size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static lv_obj_t *build_recently_played_page(struct PodcastApp *app, void *user_data) {
    (void)user_data;
    Page page = lv_page_create(tr(STR_RECENTLY_PLAYED), true,
                               page_navigator_navigate_back, &app->view->page_nav);

    lv_obj_t *cont = page.container;
    lv_obj_set_style_bg_color(cont, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);

#ifdef _WIN32
    /* PC 模拟器无 play_history(ESP-only 组件),恒为空列表。 */
    int count = 0;
#else
    int count = play_history_count();
#endif

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(cont);
        lv_label_set_text(empty, tr(STR_NO_RECENT_PLAYS));
        lv_obj_set_style_text_color(empty, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(empty, g_cjk_font, 0);
        lv_obj_add_flag(empty, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(empty, LV_PCT(100), 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        printf("[INF] Recently Played page built (empty)\n"); fflush(stdout);
        return page.screen;
    }

#ifndef _WIN32
    for (int i = 0; i < count; i++) {
        play_history_entry_t e;
        if (!play_history_get(i, &e)) continue;

        char title[PLAY_HISTORY_PATH_MAX];
        path_to_title(e.path, title, sizeof(title));

        /* 副标题:上次续播位置 mm:ss */
        int m = e.resume_sec / 60;
        int s = e.resume_sec % 60;
        char sub[32];
        snprintf(sub, sizeof(sub), "%d:%02d", m, s);

        lv_obj_t *row = lv_button_create(cont);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *title_lbl = lv_label_create(row);
        lv_label_set_text(title_lbl, title);
        lv_obj_set_style_text_font(title_lbl, g_cjk_font, 0);
        lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x333333), 0);
        lv_obj_set_flex_grow(title_lbl, 1);
        /* 长标题不换行,超宽裁剪(避免把右侧时间挤出屏幕) */
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);

        lv_obj_t *time_lbl = lv_label_create(row);
        lv_label_set_text(time_lbl, sub);
        lv_obj_set_style_text_font(time_lbl, g_cjk_font, 0);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x999999), 0);

        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, NULL);
    }
#endif /* !_WIN32 */

    printf("[INF] Recently Played page built (%d entries)\n", count); fflush(stdout);
    return page.screen;
}

void podcast_view_recently_played_init_registry(struct PodcastApp *app) {
    PAGE_REGISTE(app, PAGE_RECENTLY_PLAYED, build_recently_played_page);
}
