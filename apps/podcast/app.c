#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "esp_log.h"
#include "app_manager.h"
#include "page_navigator.h"
#include "app.h"
#include "model.h"
#include "task_store.h"
#include "view.h"
#include "controller.h"
#include "cache.h"
#include "local_cache.h"
#include "hal.h"

PodcastApp g_podcast_app;

static const char *TAG = "podcast_app";

/* Icon */
extern const lv_image_dsc_t ic_podcasts_40x40;

/* ── Startup: splash → init → navigate to network ────────────────────── */
/* (follows pc_demo/podcast/app.c podcast_app_startup pattern) */

static void podcast_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    podcast_model_init(&g_podcast_app);
    task_store_load(&g_podcast_app);   /* restore persisted download tasks */

    /* Seed the local library from .meta.json BEFORE any completion can rewrite
     * it — otherwise the first cache_local_add of the session starts from an
     * empty model and truncates every earlier session's entries. */
    cache_local_init(&g_podcast_app);

    /* Backfill: recover completed downloads missing from the index (lost to a
     * truncated/interrupted .meta.json, or boots that never loaded it). Keyed
     * by episode id so already-present entries aren't duplicated. */
    {
        PodcastModel *m = g_podcast_app.model;
        for (int i = 0; i < m->download_task_count; i++) {
            DownloadTask *t = &m->download_tasks[i];
            if (t->status == DOWNLOAD_STATUS_COMPLETED && t->episode_id > 0 &&
                !cache_local_has_episode(&g_podcast_app, t->episode_id)) {
                cache_local_add(&g_podcast_app, t->channel_id, t->channel_title,
                                t->episode_id, t->episode_title, t->audio_url,
                                t->duration_sec, t->file_path, t->collection_id);
            }
        }
    }

    if (hal_wifi_is_connected())       /* resume if WiFi already up */
        podcast_controller_resume_downloads(&g_podcast_app);
    cache_playback_init(&g_podcast_app);
    podcast_view_init(&g_podcast_app);
    g_podcast_app.view->page_nav.nav_ctx = NULL;

    /* Init HTTP stack — must be before any network request */
    cache_init();
    http_client_init();

    /* ── Do the HTTP fetch NOW, before building complex UI ──
     * This matches t_podcast's execution model: HTTP runs with minimal
     * LVGL overhead.  We've found that after the network page (tabview,
     * FABs, etc.) is built, TLS handshakes to Apple CDN timeout.
     * Hypothesis: LVGL's rendering state interferes with TCP/IP stack. */
    if (hal_wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi connected, fetching chart now...");
        podcast_controller_init(&g_podcast_app);
        g_podcast_app.controller->model = g_podcast_app.model;
        g_podcast_app.controller->view  = g_podcast_app.view;
        podcast_controller_fetch_chart(&g_podcast_app);
        /* After fetch, controller is no longer needed until view interaction.
         * View polls model->net_state to render the result. */
    } else {
        ESP_LOGI(TAG, "WiFi not connected, deferring fetch to network page");
        podcast_model_set_net_state(&g_podcast_app, NET_STATE_IDLE, NULL);
    }

    /* Show brief splash, then navigate to network page */
    lv_obj_t *splash = lv_obj_create(lv_screen_active());
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_pad_all(splash, 0, 0);
    lv_obj_set_flex_flow(splash, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(splash, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wel = lv_label_create(splash);
    lv_label_set_text(wel, "Welcome to\nNomadCast");
    lv_obj_set_style_text_color(wel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(wel, LV_TEXT_ALIGN_CENTER, 0);

    lv_refr_now(NULL); lv_timer_handler(); lv_refr_now(NULL);

    lv_obj_del(splash);
    page_navigator_navigate_to(&g_podcast_app.view->page_nav,
                               &g_podcast_app, PAGE_NETWORK, NULL);

    ESP_LOGI(TAG, "Ready");
}

static void podcast_app_stop(void)
{
    printf("[INF] podcast_app_stop\n");

    cache_playback_save();
    podcast_controller_deinit(&g_podcast_app);
    podcast_view_deinit(&g_podcast_app);
    podcast_model_deinit(&g_podcast_app);

    memset(&g_podcast_app, 0, sizeof(PodcastApp));
}

static bool podcast_app_back(void)
{
    return page_navigator_navigate_pop(&g_podcast_app.view->page_nav, &g_podcast_app);
}

/* ---- App descriptor ---- */

static application_t podcast_app_desc = {
    .name       = (char *)"Podcast",
    .icon       = &ic_podcasts_40x40,
    .start_func = podcast_app_start,
    .stop_func  = podcast_app_stop,
    .back_func  = podcast_app_back,
    .hidden     = false,
    .category   = APP_CATEGORY_TOOLS,
};

void podcast_app_register(void)
{
    app_manager_add_application(&podcast_app_desc);
}

/* Legacy API */
void podcast_app_init(void)  { /* use register + start_func instead */ }
void podcast_app_deinit(void) { /* use stop_func instead */ }
