/**
 * @file view_search_results.c
 * @brief Search results — polls model for async backend results, shows albums + tracks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_search_results.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"

extern PodcastApp g_podcast_app;

typedef struct {
    lv_obj_t *loading_label;
    lv_obj_t *main_container;
    lv_timer_t *poll_timer;
    bool content_built;
} SearchResultsPage;

static void on_channel_clicked(lv_event_t *e) {
    lv_obj_t *card = lv_event_get_current_target_obj(e);
    int channel_id = (int)(uintptr_t)lv_obj_get_user_data(card);
    int *id_ptr = (int *)malloc(sizeof(int));
    *id_ptr = channel_id;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_SEARCH_RESULTS, PAGE_CHANNEL, id_ptr);
}

static void build_results_content(lv_obj_t *parent) {
    PodcastApp *app = &g_podcast_app;
    PodcastModel *m = app->model;
    if (!m) return;

    SearchResults *sr = &m->search_results;

    /* Clear loading label */
    lv_obj_clean(parent);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_ON);

    if (sr->channel_count == 0 && sr->episode_count == 0) {
        lv_obj_t *empty = lv_label_create(parent);
        lv_label_set_text(empty, "No results found");
        lv_obj_center(empty);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x999999), 0);
        return;
    }

    /* Albums section */
    if (sr->channel_count > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Channels (%d)", sr->channel_count);
        lv_obj_t *al = lv_label_create(parent);
        lv_label_set_text(al, buf);
        lv_obj_set_style_text_font(al, g_cjk_font, 0);

        lv_obj_t *asec = lv_obj_create(parent);
        lv_obj_set_size(asec, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(asec, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_border_width(asec, 0, 0);
        lv_obj_set_style_pad_all(asec, 0, 0);
        lv_obj_set_style_pad_row(asec, 4, 0);

        for (int i = 0; i < sr->channel_count; i++) {
            Channel *channel = &sr->channels[i];
            lv_obj_t *card = lv_button_create(asec);
            lv_obj_set_size(card, LV_PCT(100), 44);
            lv_obj_t *t = lv_label_create(card);
            char info[400];
            snprintf(info, sizeof(info), "%s - %s", channel->title, channel->artist);
            lv_label_set_text(t, info);
            lv_obj_center(t);
            lv_obj_set_user_data(card, (void *)(uintptr_t)channel->id);
            lv_obj_add_event_cb(card, on_channel_clicked, LV_EVENT_CLICKED, NULL);
        }
    }

    /* Tracks section */
    if (sr->episode_count > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Episodes (%d)", sr->episode_count);
        lv_obj_t *tl = lv_label_create(parent);
        lv_label_set_text(tl, buf);
        lv_obj_set_style_text_font(tl, g_cjk_font, 0);
        lv_obj_set_style_pad_top(tl, 12, 0);

        lv_obj_t *tsec = lv_obj_create(parent);
        lv_obj_set_size(tsec, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(tsec, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_border_width(tsec, 0, 0);
        lv_obj_set_style_pad_all(tsec, 0, 0);
        lv_obj_set_style_pad_row(tsec, 4, 0);

        for (int i = 0; i < sr->episode_count; i++) {
            Episode *episode = &sr->episodes[i];
            lv_obj_t *row = lv_obj_create(tsec);
            lv_obj_set_size(row, LV_PCT(100), 36);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 4, 0);
            char info[300];
            int min = episode->duration_sec / 60;
            int sec = episode->duration_sec % 60;
            snprintf(info, sizeof(info), "%s  [%d:%02d]", episode->title, min, sec);
            lv_obj_t *tl3 = lv_label_create(row);
            lv_label_set_text(tl3, info);
            lv_obj_center(tl3);
        }
    }
}

static __attribute__((unused)) void poll_results_cb(lv_timer_t *timer) {
    SearchResultsPage *sp = (SearchResultsPage *)lv_timer_get_user_data(timer);
    if (!sp || sp->content_built) return;

    PodcastApp *app = &g_podcast_app;
    if (!app->controller || app->controller->loading_in_progress) return;

    /* Search complete — build results */
    sp->content_built = true;
    lv_timer_del(timer);
    build_results_content(sp->main_container);
    printf("[INF] Search results built\n"); fflush(stdout);
}

static lv_obj_t *build_search_results_page(struct PodcastApp *app, void *user_data) {
    (void)user_data;

    /* Free old nav_ctx if any */
    if (app->view->page_nav.nav_ctx) {
        free(app->view->page_nav.nav_ctx);
        app->view->page_nav.nav_ctx = NULL;
    }

    Page page = lv_page_create("Search Results", true, page_navigator_navigate_back, &app->view->page_nav);

    SearchResultsPage *sp = (SearchResultsPage *)calloc(1, sizeof(SearchResultsPage));
    app->view->page_nav.nav_ctx = sp;

    lv_obj_t *main = page.container;
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main, 8, 0);
    sp->main_container = main;

    /* Results are already available (search is synchronous now) */
    sp->content_built = true;
    build_results_content(main);

    return page.screen;
}

void podcast_view_search_results_init_registry(struct PodcastApp *app) {
    PAGE_REGISTE(app, PAGE_SEARCH_RESULTS, build_search_results_page);
}
