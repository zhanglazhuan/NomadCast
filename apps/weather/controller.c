#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "controller.h"
#include "app.h"
#include "model.h"
#include "view.h"
#include "weather_api.h"
#include "app_event.h"
#include "lang.h"

static const char *TAG = "weather_ctrl";

typedef enum { WCMD_STOP = 0, WCMD_LOCATE, WCMD_FETCH, WCMD_SEARCH } wcmd_t;

typedef struct {
    wcmd_t cmd;
    char   query[WEATHER_CITY_MAX];
} weather_cmd_t;

static void worker_fetch(struct WeatherApp *app);
static void worker_locate(struct WeatherApp *app);

/* Defined in apps/settings/hal_esp.c — shared across apps (podcast proves link). */
bool hal_wifi_is_connected(void);

static bool ensure_network(WeatherModel *m) {
    if (hal_wifi_is_connected()) return true;
    snprintf(m->error, sizeof(m->error), "%s", tr(STR_WEATHER_NO_NETWORK));
    return false;
}

/* Map a network outcome to a localized, specific reason so the user can tell
 * "cannot reach server" from "clock not synced" from "timed out". */
static const char *weather_err_str(weather_api_err_t e) {
    switch (e) {
    case WAPI_ERR_TLS:     return tr(STR_WEATHER_ERR_TLS);
    case WAPI_ERR_TIMEOUT: return tr(STR_WEATHER_ERR_TIMEOUT);
    case WAPI_ERR_SERVER:  return tr(STR_WEATHER_ERR_SERVER);
    case WAPI_ERR_PARSE:   return tr(STR_WEATHER_LOAD_FAILED);
    default:               return tr(STR_WEATHER_ERR_CONNECT);
    }
}

/* cJSON allocates from PSRAM to keep scarce internal DRAM free (podcast idiom). */
static void *weather_psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}

static void weather_post(struct WeatherApp *app, const weather_cmd_t *cmd) {
    WeatherController *c = app->controller;
    if (!c || !c->queue || !c->running) return;
    xQueueSend((QueueHandle_t)c->queue, cmd, 0);
}

/* ── Worker commands (run on the worker task) ─────────────────────────────── */

static void worker_locate(struct WeatherApp *app) {
    WeatherModel *m = app->model;
    if (!ensure_network(m)) return;
    double lat = 0, lon = 0;
    char city[WEATHER_CITY_MAX] = {0};
    weather_api_err_t e = weather_api_locate(&lat, &lon, city, sizeof(city));
    if (e != WAPI_OK) {
        snprintf(m->error, sizeof(m->error), "%s: %s",
                 tr(STR_WEATHER_LOCATE_FAILED), weather_err_str(e));
        return;
    }
    m->lat = lat;
    m->lon = lon;
    if (city[0]) strncpy(m->city, city, sizeof(m->city) - 1);
    m->use_ip  = true;
    m->located = true;
    weather_model_save_location(app);
    worker_fetch(app);   /* now fetch weather for the located coordinates */
}

static void worker_fetch(struct WeatherApp *app) {
    WeatherModel *m = app->model;
    if (!ensure_network(m)) return;
    if (!m->located) {
        worker_locate(app);
        return;
    }

    weather_forecast_t fc;
    memset(&fc, 0, sizeof(fc));
    weather_api_err_t e = weather_api_forecast(m->lat, m->lon, &fc);
    if (e != WAPI_OK) {
        snprintf(m->error, sizeof(m->error), "%s: %s",
                 tr(STR_WEATHER_LOAD_FAILED), weather_err_str(e));
        return;
    }

    m->temp       = fc.temp;
    m->feels_like = fc.feels_like;
    m->humidity   = fc.humidity;
    m->wind       = fc.wind;
    strncpy(m->text, fc.text, sizeof(m->text) - 1);
    m->text[sizeof(m->text) - 1] = '\0';
    for (int i = 0; i < WEATHER_DAYS; i++) {
        m->tmax[i]       = fc.tmax[i];
        m->tmin[i]       = fc.tmin[i];
        strncpy(m->daily_text[i], fc.daily_text[i], sizeof(m->daily_text[i]) - 1);
        m->daily_text[i][sizeof(m->daily_text[i]) - 1] = '\0';
        strncpy(m->daily_date[i], fc.daily_date[i], sizeof(m->daily_date[i]) - 1);
    }
    m->has_data = true;
    m->error[0] = '\0';

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(m->updated, sizeof(m->updated), "%H:%M", &tmv);
}

static void worker_search(struct WeatherApp *app, const char *query) {
    WeatherModel *m = app->model;
    if (!ensure_network(m)) return;
    weather_geo_result_t results[WEATHER_SEARCH_MAX];
    int count = 0;
    weather_api_err_t e = weather_api_geocode(query, results, WEATHER_SEARCH_MAX, &count);
    if (e != WAPI_OK) {
        m->search_count = 0;
        m->search_done  = true;
        snprintf(m->error, sizeof(m->error), "%s: %s",
                 tr(STR_WEATHER_LOAD_FAILED), weather_err_str(e));
        return;
    }
    for (int i = 0; i < count && i < WEATHER_SEARCH_MAX; i++) {
        strncpy(m->search_name[i], results[i].name, WEATHER_CITY_MAX - 1);
        m->search_lat[i] = results[i].lat;
        m->search_lon[i] = results[i].lon;
    }
    m->search_count = count;
    m->search_done  = true;
    m->error[0] = '\0';
}

static void weather_worker(void *arg) {
    struct WeatherApp *app = (struct WeatherApp *)arg;
    WeatherController *c = app->controller;
    WeatherModel *m = app->model;

    weather_cmd_t cmd;
    while (c->running) {
        if (!xQueueReceive((QueueHandle_t)c->queue, &cmd, pdMS_TO_TICKS(250))) continue;
        if (cmd.cmd == WCMD_STOP) break;

        m->busy = true;
        switch (cmd.cmd) {
        case WCMD_LOCATE: worker_locate(app);         break;
        case WCMD_FETCH:  worker_fetch(app);          break;
        case WCMD_SEARCH: worker_search(app, cmd.query); break;
        default: break;
        }
        m->busy = false;
        /* Write-before-fire gives the LVGL-thread reader a happens-before edge. */
        app_event_fire(APP_EVENT_WEATHER_UPDATED, NULL);
    }

    c->task = NULL;
    vTaskDelete(NULL);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

void weather_controller_init(struct WeatherApp *app) {
    WeatherController *c = (WeatherController *)calloc(1, sizeof(WeatherController));
    if (!c) {
        ESP_LOGE(TAG, "controller alloc failed");
        return;
    }
    app->controller = c;
    c->model = NULL;
    c->view  = NULL;
    c->queue = xQueueCreate(4, sizeof(weather_cmd_t));

    cJSON_Hooks hooks = {
        .malloc_fn = weather_psram_malloc,
        .free_fn   = free,
    };
    cJSON_InitHooks(&hooks);

    c->running = true;
    if (xTaskCreatePinnedToCore(weather_worker, "weather", 8192, app, 5,
                                (TaskHandle_t *)&c->task, 1) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        c->running = false;
    }
    ESP_LOGI(TAG, "init done");
}

void weather_controller_deinit(struct WeatherApp *app) {
    WeatherController *c = app->controller;
    if (!c) return;

    c->running = false;
    if (c->queue) {
        weather_cmd_t stop = { .cmd = WCMD_STOP };
        xQueueSend((QueueHandle_t)c->queue, &stop, 0);
    }
    /* Wait for the worker to observe running=false and exit. A network op can
     * block up to ~15 s (locate + fetch can chain), so allow generous headroom. */
    for (int i = 0; i < 18000 && c->task; i++) vTaskDelay(pdMS_TO_TICKS(10));

    if (c->queue) {
        vQueueDelete((QueueHandle_t)c->queue);
        c->queue = NULL;
    }
    free(c);
    app->controller = NULL;
    ESP_LOGI(TAG, "deinit done");
}

/* ── UI-thread entry points ────────────────────────────────────────────────── */

void weather_controller_auto_refresh(struct WeatherApp *app) {
    if (!app || !app->model) return;
    weather_cmd_t cmd = { .cmd = app->model->located ? WCMD_FETCH : WCMD_LOCATE };
    weather_post(app, &cmd);
}

void weather_controller_refresh(struct WeatherApp *app) {
    weather_cmd_t cmd = { .cmd = WCMD_FETCH };
    weather_post(app, &cmd);
}

void weather_controller_locate(struct WeatherApp *app) {
    weather_cmd_t cmd = { .cmd = WCMD_LOCATE };
    weather_post(app, &cmd);
}

void weather_controller_search(struct WeatherApp *app, const char *query) {
    if (!query || !query[0]) return;
    weather_cmd_t cmd = { .cmd = WCMD_SEARCH };
    strncpy(cmd.query, query, sizeof(cmd.query) - 1);
    weather_post(app, &cmd);
}

void weather_controller_apply_city(struct WeatherApp *app, int idx) {
    WeatherModel *m = app->model;
    if (!m || idx < 0 || idx >= m->search_count) return;
    strncpy(m->city, m->search_name[idx], sizeof(m->city) - 1);
    m->lat     = m->search_lat[idx];
    m->lon     = m->search_lon[idx];
    m->use_ip  = false;
    m->located = true;
    weather_model_save_location(app);
    weather_controller_refresh(app);
}

void weather_nav_push(struct WeatherApp *app, int from, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    PAGE_NAVIGATE_TO(app, from, to, NULL);
}

void weather_nav_replace(struct WeatherApp *app, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    page_navigator_navigate_to(&app->view->page_nav, app, to, NULL);
}
