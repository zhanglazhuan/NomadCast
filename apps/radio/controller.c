#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "controller.h"
#include "app.h"
#include "model.h"
#include "view.h"
#include "radio_api.h"
#include "audio_player.h"
#include "input.h"
#include "lv_toast.h"
#include "lang.h"
#include "app_event.h"
#include "server_config.h"

static const char *TAG = "radio_ctrl";

typedef enum { RCMD_STOP = 0, RCMD_REGIONS, RCMD_STATIONS } rcmd_t;

typedef struct {
    rcmd_t cmd;
    char   country[4];
    char   region[RADIO_REGION_KEY_MAX];
} radio_cmd_t;

/* Defined in apps/settings/hal_esp.c — shared across apps (podcast proves link). */
bool hal_wifi_is_connected(void);

static void on_radio_input(input_event_t event, void *user_data);

static bool ensure_network(RadioModel *m) {
    if (hal_wifi_is_connected()) return true;
    snprintf(m->error, sizeof(m->error), "%s", tr(STR_RADIO_NO_NETWORK));
    return false;
}

static const char *radio_err_str(radio_api_err_t e) {
    switch (e) {
    case RAPI_ERR_SERVER: return tr(STR_RADIO_LOAD_FAILED);
    case RAPI_ERR_PARSE:  return tr(STR_RADIO_LOAD_FAILED);
    default:              return tr(STR_RADIO_NO_NETWORK);
    }
}

/* ── Worker commands (run on the worker task) ─────────────────────────────── */

static void worker_regions(RadioApp *app, const char *country) {
    RadioModel *m = app->model;
    if (!ensure_network(m)) return;
    int count = 0;
    radio_api_err_t e = radio_api_regions(country, m->regions, RADIO_MAX_REGIONS, &count);
    if (e != RAPI_OK) {
        m->region_count = 0;
        snprintf(m->error, sizeof(m->error), "%s: %s",
                 tr(STR_RADIO_LOAD_FAILED), radio_err_str(e));
        return;
    }
    m->region_count = count;
    m->error[0] = '\0';
}

static void worker_stations(RadioApp *app, const char *country, const char *region) {
    RadioModel *m = app->model;
    if (!ensure_network(m)) return;
    int count = 0, total = 0;
    radio_api_err_t e = radio_api_stations(country, region, m->stations,
                                           RADIO_MAX_STATIONS, &count, &total);
    if (e != RAPI_OK) {
        m->station_count = 0;
        snprintf(m->error, sizeof(m->error), "%s: %s",
                 tr(STR_RADIO_LOAD_FAILED), radio_err_str(e));
        return;
    }
    m->station_count = count;
    m->station_total = total;
    m->error[0] = '\0';
}

static void radio_worker(void *arg) {
    RadioApp *app = (RadioApp *)arg;
    RadioController *c = app->controller;
    RadioModel *m = app->model;

    radio_cmd_t cmd;
    while (c->running) {
        if (!xQueueReceive((QueueHandle_t)c->queue, &cmd, pdMS_TO_TICKS(250))) continue;
        if (cmd.cmd == RCMD_STOP) break;

        m->busy = true;
        m->error[0] = '\0';
        switch (cmd.cmd) {
        case RCMD_REGIONS:  worker_regions(app, cmd.country); break;
        case RCMD_STATIONS: worker_stations(app, cmd.country, cmd.region); break;
        default: break;
        }
        m->busy = false;
        /* Write-before-fire gives the LVGL-thread reader a happens-before edge. */
        app_event_fire(APP_EVENT_RADIO_UPDATED, NULL);
    }

    c->task = NULL;
    vTaskDelete(NULL);
}

static void radio_post(RadioApp *app, const radio_cmd_t *cmd) {
    RadioController *c = app->controller;
    if (!c || !c->queue || !c->running) return;
    xQueueSend((QueueHandle_t)c->queue, cmd, 0);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

void radio_controller_init(RadioApp *app) {
    RadioController *c = (RadioController *)calloc(1, sizeof(RadioController));
    if (!c) {
        ESP_LOGE(TAG, "controller alloc failed");
        return;
    }
    app->controller = c;
    c->model = NULL;
    c->view  = NULL;
    c->queue = xQueueCreate(4, sizeof(radio_cmd_t));

    c->running = true;
    if (xTaskCreatePinnedToCore(radio_worker, "radio", 8192, app, 5,
                                (TaskHandle_t *)&c->task, 1) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        c->running = false;
    }

    input_subscribe(on_radio_input, app);
    ESP_LOGI(TAG, "init done");
}

void radio_controller_deinit(RadioApp *app) {
    RadioController *c = app->controller;
    if (!c) return;

    /* Stop any live stream before tearing down the app. */
    if (audio_player_is_active()) audio_player_stop();

    input_unsubscribe(on_radio_input);

    c->running = false;
    if (c->queue) {
        radio_cmd_t stop = { .cmd = RCMD_STOP };
        xQueueSend((QueueHandle_t)c->queue, &stop, 0);
    }
    /* Wait for the worker to observe running=false and exit. */
    for (int i = 0; i < 3000 && c->task; i++) vTaskDelay(pdMS_TO_TICKS(10));

    if (c->queue) {
        vQueueDelete((QueueHandle_t)c->queue);
        c->queue = NULL;
    }
    free(c);
    app->controller = NULL;
    ESP_LOGI(TAG, "deinit done");
}

/* ── Physical key handling (posted from the button task) ──────────────────── */

static void async_next(void *ud)   { radio_controller_next((RadioApp *)ud); }
static void async_prev(void *ud)   { radio_controller_prev((RadioApp *)ud); }
static void async_toggle(void *ud) { radio_controller_toggle((RadioApp *)ud); }

static void on_radio_input(input_event_t event, void *user_data) {
    switch (event) {
    case INPUT_EVENT_NEXT_TRACK: lv_async_call(async_next, user_data);   break;
    case INPUT_EVENT_PREV_TRACK: lv_async_call(async_prev, user_data);   break;
    case INPUT_EVENT_PLAY_PAUSE: lv_async_call(async_toggle, user_data); break;
    default: break;
    }
}

/* ── Active-list helpers ──────────────────────────────────────────────────── */

static int list_count(const RadioModel *m) {
    return m->play_source == RADIO_SRC_FAVORITES ? m->favorite_count : m->station_count;
}

static radio_station_t *list_at(RadioModel *m, int i) {
    return m->play_source == RADIO_SRC_FAVORITES ? &m->favorites[i] : &m->stations[i];
}

static radio_station_t *current_station(RadioModel *m) {
    if (!m || !m->playing_valid) return NULL;
    return &m->playing;
}

/* ── UI-thread entry points ────────────────────────────────────────────────── */

void radio_controller_load_regions(RadioApp *app) {
    if (!app || !app->model) return;
    radio_cmd_t cmd = { .cmd = RCMD_REGIONS };
    strncpy(cmd.country, app->model->country, sizeof(cmd.country) - 1);
    radio_post(app, &cmd);
}

void radio_controller_load_stations(RadioApp *app,
                                    const char *region_key, const char *region_name) {
    RadioModel *m = app->model;
    if (!m || !region_key) return;

    strncpy(m->region_key, region_key, sizeof(m->region_key) - 1);
    m->region_key[sizeof(m->region_key) - 1] = '\0';
    if (region_name) {
        strncpy(m->region_name, region_name, sizeof(m->region_name) - 1);
        m->region_name[sizeof(m->region_name) - 1] = '\0';
    } else {
        m->region_name[0] = '\0';
    }
    m->station_count = 0;
    m->error[0] = '\0';

    radio_cmd_t cmd = { .cmd = RCMD_STATIONS };
    strncpy(cmd.country, m->country, sizeof(cmd.country) - 1);
    strncpy(cmd.region, region_key, sizeof(cmd.region) - 1);
    radio_post(app, &cmd);
}

void radio_controller_play(RadioApp *app, int src, int idx) {
    RadioModel *m = app->model;
    if (!m) return;
    if (src < RADIO_SRC_STATIONS || src > RADIO_SRC_FAVORITES) return;
    if (src == RADIO_SRC_FAVORITES && (idx < 0 || idx >= m->favorite_count)) return;
    if (src == RADIO_SRC_STATIONS  && (idx < 0 || idx >= m->station_count))  return;

    m->play_source = src;
    m->current = idx;

    radio_station_t *s = list_at(m, idx);
    if (!s) return;

    /* Snapshot the station so the player screen and favorite toggle keep a
     * stable reference even after the source list (favorites) mutates. */
    m->playing = *s;
    m->playing_valid = true;

    /* HLS (m3u8) and MP3/AAC alike go through the proxy; the audio player's
     * http_stream element parses playlists (enable_playlist_parser) and chains
     * live segments, so no special-casing is needed here. */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/radio/stream?uuid=%s",
             NOMADCAST_SERVER_BASE, s->uuid);

    audio_player_init();
    bool ok = audio_player_play(url);
    if (!ok) lv_toast_show(tr(STR_RADIO_FAILED_TO_PLAY), 2000);
}

void radio_controller_toggle(RadioApp *app) {
    (void)app;
    if (audio_player_is_playing()) {
        audio_player_pause(true);
    } else if (audio_player_is_active()) {
        audio_player_pause(false);
    }
}

void radio_controller_next(RadioApp *app) {
    RadioModel *m = app->model;
    if (!m) return;
    int n = list_count(m);
    if (n == 0) return;
    int idx = (m->current < 0) ? 0 : (m->current + 1) % n;
    radio_controller_play(app, m->play_source, idx);
}

void radio_controller_prev(RadioApp *app) {
    RadioModel *m = app->model;
    if (!m) return;
    int n = list_count(m);
    if (n == 0) return;
    int idx = (m->current < 0) ? 0 : (m->current - 1 + n) % n;
    radio_controller_play(app, m->play_source, idx);
}

void radio_controller_toggle_favorite(RadioApp *app) {
    RadioModel *m = app->model;
    if (!m) return;
    radio_station_t *s = current_station(m);
    if (!s) return;
    bool now_fav = radio_model_toggle_favorite(app, s);
    lv_toast_show(tr(now_fav ? STR_RADIO_FAVORITED : STR_RADIO_UNFAVORITED), 1500);
    /* Rebuild the current page so the favorite button/list reflects the change. */
    app_event_fire(APP_EVENT_RADIO_UPDATED, NULL);
}

/* ── Navigation helpers ────────────────────────────────────────────────────── */

void radio_nav_push(RadioApp *app, int from, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    PAGE_NAVIGATE_TO(app, from, to, NULL);
}

void radio_nav_replace(RadioApp *app, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    page_navigator_navigate_to(&app->view->page_nav, app, to, NULL);
}
