/**
 * @file controller.c — Podcast controller (ESP32 only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "controller.h"
#include "app.h"
#include "app_event.h"
#include "model.h"
#include "task_store.h"
#include "backend.h"
#include "cache.h"
#include "local_cache.h"
#include "hal.h"
#include "flash_store.h"
#include "lv_status_bar.h"
#include "lv_toast.h"
#include "sleep_monitor.h"
#include "i_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ff.h"      /* FatFS f_getfree — SD-mount check */
#include "cJSON.h"

static const char *TAG = "podcast_ctrl";

/* ══════════════════════════════════════════════════════════════════════════
 * Controller private context — allocated on init, freed on deinit.
 * All mutable state lives here; no static globals.
 * ══════════════════════════════════════════════════════════════════════════ */

#define DL_DONE_RING         32
#define RSS_BLACKLIST_MAX     32

/* ── Download worker state ──────────────────────────────────────────── */
typedef struct {
    TaskHandle_t      task_handle;
    SemaphoreHandle_t sem;
    bool              running;
    volatile int      speed_bps;
    volatile bool     active;
    volatile int      avg_bps;
    volatile int      cur_remaining;
    volatile int      bytes_per_audio_sec;
    volatile bool     status_dirty;
    volatile int      current_task_id;
    volatile bool     abort_current;
    volatile bool     yield_to_audio;
    volatile bool     user_paused;
    volatile int      done_ids[DL_DONE_RING];
    volatile int      done_head;
    volatile int      done_tail;
} DownloadCtx;

/* ── Controller private context ─────────────────────────────────────── */
typedef struct PodcastCtrlCtx {
    /* RSS feed bridge */
    char       feed_url[2048];
    rss_feed_t feed_result;
    bool       feed_pending, feed_done, feed_ok;
    int        feed_channel_id;
    int        feed_offset;          /* pagination offset (0-indexed) */
    int        feed_limit;           /* pagination page size */
    int        rss_blacklist[RSS_BLACKLIST_MAX];
    int        rss_blacklist_count;
    SemaphoreHandle_t rss_sem;
    TaskHandle_t rss_task;
    volatile bool rss_running;
    /* Download worker */
    DownloadCtx dl;
} PodcastCtrlCtx;

extern PodcastApp g_podcast_app;

/* Helper: get ctx from app */
static inline PodcastCtrlCtx *ctl_ctx(struct PodcastApp *app) {
    return app && app->controller ? app->controller->ctx : NULL;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void s_strcpy(char *dst, const char *src, size_t sz) {
    if (sz > 0) { strncpy(dst, src ? src : "", sz - 1); dst[sz - 1] = '\0'; }
}

static void js_str(cJSON *obj, const char *key, char *dst, int max) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && item->type == cJSON_String && item->valuestring)
        s_strcpy(dst, item->valuestring, max);
    else dst[0] = '\0';
}

/* Apple Podcasts region — persisted in flash by the Settings → Country dropdown.
 * Read per request so a changed setting applies on the next fetch/refresh. */
static void podcast_country(char *out, int sz) {
    flash_get_str("podcast", "country", out, (size_t)sz, "cn");
}

/* ── Genre → category mapping (English + Chinese) ──────────────────────── */

static channel_category_t map_genre(const char *genre)
{
    if (!genre || !genre[0]) return CHANNEL_CATEGORY_OTHERS;

    /* English keywords */
    if (strstr(genre, "News") || strstr(genre, "Society") ||
        strstr(genre, "Politics"))
        return CHANNEL_CATEGORY_NEWS_SOCIETY;
    if (strstr(genre, "Business") || strstr(genre, "Technology") ||
        strstr(genre, "Tech") || strstr(genre, "Science"))
        return CHANNEL_CATEGORY_BUSINESS_TECH;
    if (strstr(genre, "Arts") || strstr(genre, "History") ||
        strstr(genre, "Music") || strstr(genre, "Fiction") ||
        strstr(genre, "Books") || strstr(genre, "Literature"))
        return CHANNEL_CATEGORY_ARTS_HISTORY;
    if (strstr(genre, "Comedy") || strstr(genre, "Health") ||
        strstr(genre, "Fitness") || strstr(genre, "Leisure") ||
        strstr(genre, "Hobbies") || strstr(genre, "Home"))
        return CHANNEL_CATEGORY_COMEDY_LIFE;
    if (strstr(genre, "True Crime") || strstr(genre, "Crime") ||
        strstr(genre, "Education") || strstr(genre, "Courses") ||
        strstr(genre, "Religion") || strstr(genre, "Spirituality"))
        return CHANNEL_CATEGORY_CRIME_EDU;

    /* Chinese keywords — Apple CN API returns Chinese genre names */
    if (strstr(genre, "新闻") || strstr(genre, "社会") || strstr(genre, "政治"))
        return CHANNEL_CATEGORY_NEWS_SOCIETY;
    if (strstr(genre, "商务") || strstr(genre, "科技") || strstr(genre, "科学"))
        return CHANNEL_CATEGORY_BUSINESS_TECH;
    if (strstr(genre, "艺术") || strstr(genre, "历史") || strstr(genre, "音乐") ||
        strstr(genre, "小说") || strstr(genre, "文学"))
        return CHANNEL_CATEGORY_ARTS_HISTORY;
    if (strstr(genre, "喜剧") || strstr(genre, "健康") || strstr(genre, "休闲"))
        return CHANNEL_CATEGORY_COMEDY_LIFE;
    if (strstr(genre, "犯罪") || strstr(genre, "教育") || strstr(genre, "宗教"))
        return CHANNEL_CATEGORY_CRIME_EDU;

    return CHANNEL_CATEGORY_OTHERS;
}

/* ── RSS fetch bridge ────────────────────────────────────────────────── */

static bool rss_is_blacklisted(PodcastCtrlCtx *ctx, int channel_id) {
    for (int i = 0; i < ctx->rss_blacklist_count; i++)
        if (ctx->rss_blacklist[i] == channel_id) return true;
    return false;
}

static void rss_blacklist_add(PodcastCtrlCtx *ctx, int channel_id) {
    if (rss_is_blacklisted(ctx, channel_id)) return;
    if (ctx->rss_blacklist_count >= RSS_BLACKLIST_MAX) return;
    ctx->rss_blacklist[ctx->rss_blacklist_count++] = channel_id;
    ESP_LOGW(TAG, "Channel %d blacklisted (RSS unreachable)", channel_id);
}

bool g_rss_done(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->feed_done : false;
}
bool g_rss_ok(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->feed_ok : false;
}
rss_feed_t *g_rss_result(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? &ctx->feed_result : NULL;
}
int g_rss_channel_id(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->feed_channel_id : 0;
}

void controller_rss_reparse(int offset, int limit) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    if (!ctx) return;
    ctx->feed_offset = offset;
    ctx->feed_limit  = limit;
    ctx->feed_done   = false;
    ctx->feed_ok     = false;
    ctx->feed_pending = true;
    if (ctx->rss_sem) xSemaphoreGive(ctx->rss_sem);
}

/* ── WiFi event listener — updates podcast model net_state ──────────── */

static void dl_resume_pending(PodcastApp *app);
static void dl_worker_start(PodcastApp *app);
static void rss_worker_task(void *arg) {
    PodcastCtrlCtx *c = (PodcastCtrlCtx *)arg;
    while (c->rss_running) {
        if (xSemaphoreTake(c->rss_sem, portMAX_DELAY) == pdTRUE && c->rss_running)
            controller_process_rss();
    }
    c->rss_task = NULL;
    vTaskDelete(NULL);
}

static void on_wifi_event(app_event_t event, const void *data)
{
    (void)data;
    if (event == APP_EVENT_WIFI_DISCONNECTED) {
        podcast_model_set_net_state(&g_podcast_app, NET_STATE_OFFLINE, NULL);
    } else if (event == APP_EVENT_WIFI_CONNECTED) {
        /* WiFi back — resume any pending downloads that survived a reboot */
        dl_resume_pending(&g_podcast_app);
    } else if (event == APP_EVENT_KEY_PLAY_PAUSE) {
        podcast_controller_toggle_play_pause(&g_podcast_app);
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

static void *psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}

/* Auto power-off safety gate: allow shutdown only when nothing is playing
 * (headphone and speaker share the same ADF pipeline, so one check covers both)
 * and no download is queued or in flight. Registered with sleep_monitor by
 * podcast_controller_init. */
static bool ctl_auto_power_off_allowed(void)
{
    if (audio_player_is_playing()) return false;

    PodcastApp *app = &g_podcast_app;
    if (!app || !app->model) return true;
    for (int i = 0; i < app->model->download_task_count; i++) {
        download_status_t s = app->model->download_tasks[i].status;
        if (s == DOWNLOAD_STATUS_PENDING || s == DOWNLOAD_STATUS_DOWNLOADING)
            return false;
    }
    return true;
}

void podcast_controller_init(struct PodcastApp *app) {
    if (!app) return;
    /* Force cJSON to use PSRAM for all allocations.  Chart JSON is ~25KB
     * and creates 400+ small nodes (~22KB).  Default malloc routes small
     * allocations to internal DRAM (~50KB total) → OOM → parse fail. */
    static cJSON_Hooks psram_hooks = {
        .malloc_fn = (void *(*)(size_t))psram_malloc,
        .free_fn   = free,
    };
    cJSON_InitHooks(&psram_hooks);

    app->controller = (PodcastController *)calloc(1, sizeof(PodcastController));
    if (!app->controller) { ESP_LOGE(TAG, "controller OOM"); return; }

    /* Allocate private context — all mutable state lives here, freed on deinit */
    PodcastCtrlCtx *ctx = (PodcastCtrlCtx *)calloc(1, sizeof(PodcastCtrlCtx));
    if (!ctx) { free(app->controller); app->controller = NULL; ESP_LOGE(TAG, "controller ctx OOM"); return; }
    ctx->dl.current_task_id = -1;
    ctx->rss_running = true;
    ctx->rss_sem = xSemaphoreCreateCounting(8, 0);
    if (!ctx->rss_sem) { free(ctx); free(app->controller); app->controller = NULL; return; }
    app->controller->ctx = ctx;

    if (xTaskCreatePinnedToCore(rss_worker_task, "podcast_rss", 8192, ctx, 4, &ctx->rss_task, 1) != pdPASS) {
        vSemaphoreDelete(ctx->rss_sem); free(ctx); free(app->controller); app->controller = NULL; return;
    }

    backend_init();
    app_event_register(on_wifi_event);

    /* Auto power-off must not fire while playback or a download is active. */
    sleep_monitor_set_power_off_check(ctl_auto_power_off_allowed);
}

void podcast_controller_deinit(struct PodcastApp *app) {
    if (app->controller) {
        PodcastCtrlCtx *ctx = app->controller->ctx;
        /* Stop download worker */
        if (ctx) {
            ctx->rss_running = false;
            if (ctx->rss_sem) xSemaphoreGive(ctx->rss_sem);
            for (int i = 0; i < 18000 && ctx->rss_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
            if (ctx->rss_sem) { vSemaphoreDelete(ctx->rss_sem); ctx->rss_sem = NULL; }
            ctx->dl.running = false;
            ctx->dl.abort_current = true;
            if (ctx->dl.sem) xSemaphoreGive(ctx->dl.sem);
            for (int i = 0; i < 18000 && ctx->dl.task_handle; i++)
                vTaskDelay(pdMS_TO_TICKS(10));
            if (ctx->dl.sem) {
                vSemaphoreDelete(ctx->dl.sem);
                ctx->dl.sem = NULL;
            }
            free(ctx);
            app->controller->ctx = NULL;
        }
        backend_deinit();
        app_event_unregister(on_wifi_event);
        free(app->controller);
        app->controller = NULL;
    }
}

/* ── Network load ────────────────────────────────────────────────────── */

/* ── Network content loading ──────────────────────────────────────────── */
/* Data is pre-loaded at app startup (podcast_app_start).
 * This function only checks WiFi — the actual fetch is in app.c. */

bool podcast_controller_load_network_content(struct PodcastApp *app) {
    if (!app || !app->controller || !hal_wifi_is_connected()) {
        if (app) podcast_model_set_net_state(app, NET_STATE_OFFLINE, NULL);
        return false;
    }
    return true;
}

/* Apple Podcasts genre IDs mapped to our categories (primary genre per cat). */
static int category_to_genre_id(channel_category_t cat) {
    switch (cat) {
        case CHANNEL_CATEGORY_NEWS_SOCIETY:   return 1310;  /* News */
        case CHANNEL_CATEGORY_BUSINESS_TECH:  return 1316;  /* Technology */
        case CHANNEL_CATEGORY_ARTS_HISTORY:   return 1301;  /* Arts */
        case CHANNEL_CATEGORY_COMEDY_LIFE:    return 1325;  /* Leisure */
        case CHANNEL_CATEGORY_CRIME_EDU:      return 1304;  /* Education */
        default:                              return 0;
    }
}

bool podcast_controller_fetch_chart(struct PodcastApp *app)
{
    return podcast_controller_fetch_chart_by_category(app, CHANNEL_CATEGORY_COUNT);
}

bool podcast_controller_fetch_chart_by_category(struct PodcastApp *app, int cat)
{
    bk_channel_t *bk = NULL;
    int count = 0;

    char cc[8]; podcast_country(cc, sizeof(cc));
    int genre_id = (cat >= 0 && cat < CHANNEL_CATEGORY_COUNT)
                   ? category_to_genre_id((channel_category_t)cat) : 0;

    count = backend_fetch_chart(&bk, cc, 50, genre_id);

    if (count <= 0) {
        podcast_model_set_net_state(app, NET_STATE_ERROR,
            hal_wifi_is_connected() ? "Server unreachable" : "No network connection");
        ESP_LOGW(TAG, "fetch_chart: 0 channels (genre=%d)", genre_id);
        return false;
    }

    /* Convert bk_channel_t → Channel, import into model */
    Channel *channels = (Channel *)heap_caps_calloc(count, sizeof(Channel), MALLOC_CAP_SPIRAM);
    if (!channels) {
        ESP_LOGE(TAG, "fetch_chart: alloc %d channels (%u bytes) FAILED",
                 count, (unsigned)(count * sizeof(Channel)));
        free(bk);
        podcast_model_set_net_state(app, NET_STATE_ERROR,
            "Memory allocation failed");
        return false;
    }

    for (int i = 0; i < count; i++) {
        Channel *c = &channels[i];
        bk_channel_t *b = &bk[i];
        c->id = i + 1;
        c->collection_id = b->collection_id;
        snprintf(c->title, sizeof(c->title), "%s", b->title);
        snprintf(c->artist, sizeof(c->artist), "%s", b->artist);
        snprintf(c->feed_url, sizeof(c->feed_url), "%s", b->feed_url);
        snprintf(c->artwork_url, sizeof(c->artwork_url), "%s", b->artwork_url);
        snprintf(c->genre, sizeof(c->genre), "%s", b->genre);
        c->episode_count = b->episode_count;
        c->category = map_genre(c->genre);
        c->downloaded = false;
        c->card_color = 0xE91E63 + (i * 0x12345) % 0x1000000;
        /* Extract date portion from ISO 8601 release_date */
        if (b->release_date[0]) {
            const char *t = strchr(b->release_date, 'T');
            int n = t ? (int)(t - b->release_date) : (int)strlen(b->release_date);
            if (n >= (int)sizeof(c->upload_time)) n = (int)sizeof(c->upload_time) - 1;
            memcpy(c->upload_time, b->release_date, n);
            c->upload_time[n] = '\0';
        }
    }
    free(bk);

    podcast_model_import_channels(app, channels, count);
    podcast_model_set_net_state(app, NET_STATE_READY, NULL);
    ESP_LOGI(TAG, "Loaded %d channels", count);

    /* Pre-download artwork to SD card cache.
     * channels is owned by the model now (import_channels stores the pointer);
     * do NOT free it here — model_deinit handles cleanup. */
    for (int i = 0; i < count; i++) {
        if (channels[i].artwork_url[0]) {
            cache_artwork_download(channels[i].collection_id, channels[i].artwork_url);
        }
    }

    return true;
}

bool podcast_controller_load_more_channels(struct PodcastApp *app, channel_category_t cat)
{ return podcast_model_load_more_network_channels(app, cat); }

/* ── Search ──────────────────────────────────────────────────────────── */

bool podcast_controller_search(struct PodcastApp *app, const char *q) {
    if (!app || !q || !q[0]) return false;
    podcast_model_add_search_history(app, q);
    char cc[8]; podcast_country(cc, sizeof(cc));
    bk_channel_t *bk = NULL; int n = backend_search_podcasts_sync(&bk, q, cc, 20);
    if (!bk || n <= 0) { podcast_model_clear_search_results(app); return false; }
    Channel *ch = (Channel *)heap_caps_calloc(n, sizeof(Channel), MALLOC_CAP_SPIRAM);
    if (!ch) { free(bk); podcast_model_clear_search_results(app); return false; }
    for (int i = 0; i < n; i++) {
        ch[i].id = 1000 + i; ch[i].collection_id = bk[i].collection_id;
        s_strcpy(ch[i].title, bk[i].title, sizeof(ch[i].title));
        s_strcpy(ch[i].artist, bk[i].artist, sizeof(ch[i].artist));
        s_strcpy(ch[i].feed_url, bk[i].feed_url, sizeof(ch[i].feed_url));
    }
    free(bk);
    podcast_model_set_search_results(app, ch, n, NULL, 0, q);
    return true;
}

/* ── Channel episodes ────────────────────────────────────────────────── */

bool podcast_controller_fetch_channel_episodes(struct PodcastApp *app, int cid) {
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    const Channel *ch = podcast_model_get_channel_by_id(app, cid);
    if (!ctx || !ch) return false;

    /* Blacklist: skip HTTP fetch for channels that previously returned 502/unreachable */
    if (rss_is_blacklisted(ctx, cid)) {
        memset(&ctx->feed_result, 0, sizeof(ctx->feed_result));
        s_strcpy(ctx->feed_result.error, "Feed unavailable (blacklisted)",
                 sizeof(ctx->feed_result.error));
        ctx->feed_done = true;
        ctx->feed_ok = false;
        ctx->feed_channel_id = cid;
        ESP_LOGI(TAG, "Channel %d blacklisted — skipping fetch", cid);
        return true;
    }

    memset(&ctx->feed_result, 0, sizeof(ctx->feed_result));
    ctx->feed_done = ctx->feed_ok = false;
    ctx->feed_channel_id = cid;
    ctx->feed_offset = 0;
    ctx->feed_limit  = EPISODES_PER_PAGE;
    if (ch->feed_url[0]) {
        snprintf(ctx->feed_url, sizeof(ctx->feed_url), "%s", ch->feed_url);
    } else {
        snprintf(ctx->feed_url, sizeof(ctx->feed_url), "SEARCH:%s", ch->title);
    }
    ctx->feed_pending = true;
    if (ctx->rss_sem) xSemaphoreGive(ctx->rss_sem);
    return true;
}

void controller_process_rss(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    if (!ctx || !ctx->feed_pending) return;
    ctx->feed_pending = false;

    if (strncmp(ctx->feed_url, "SEARCH:", 7) == 0) {
        char cc[8]; podcast_country(cc, sizeof(cc));
        bk_channel_t *r = NULL; int n = backend_search_podcasts_sync(&r, ctx->feed_url + 7, cc, 3);
        if (r && n > 0) {
            for (int i = 0; i < n; i++) {
                if (r[i].feed_url[0]) {
                    snprintf(ctx->feed_url, sizeof(ctx->feed_url), "%s", r[i].feed_url);
                    free(r); ctx->feed_pending = true;
                    if (ctx->rss_sem) xSemaphoreGive(ctx->rss_sem);
                    return;
                }
            }
            free(r);
        }
        ctx->feed_done = true; ctx->feed_ok = false;
        rss_blacklist_add(ctx, ctx->feed_channel_id);
        return;
    }

    /*
     * Route RSS fetch through the podcast proxy server.
     * Server does HTTPS + XML parse → returns clean JSON.
     * ESP32 only does one HTTP call and parses simple JSON.
     */
    {
        char url[2560];
        const Channel *ch = podcast_model_get_channel_by_id(&g_podcast_app, ctx->feed_channel_id);
        int col_id = ch ? ch->collection_id : 0;
        snprintf(url, sizeof(url),
                 "%s/api/episodes?feed_url=%s&collection_id=%d&offset=%d&limit=%d",
                 PODCAST_SERVER, ctx->feed_url, col_id, ctx->feed_offset, ctx->feed_limit);
        int st, len;
        char *body = http_get_sync(url, &st, &len);
        memset(&ctx->feed_result, 0, sizeof(ctx->feed_result));

        if (!body) {
            const char *detail = http_last_error();
            s_strcpy(ctx->feed_result.error,
                     detail && detail[0] ? detail : "Server unreachable",
                     sizeof(ctx->feed_result.error));
            ctx->feed_ok = false;
            ctx->feed_done = true;
            rss_blacklist_add(ctx, ctx->feed_channel_id);
            return;
        }

        ESP_LOGI(TAG, "RSS proxy: HTTP %d, %d bytes", st, len);

        /* Check for server error — response may contain {"error": "..."} */
        if (st != 200) {
            cJSON *root = cJSON_Parse(body);
            if (root) {
                cJSON *e = cJSON_GetObjectItem(root, "error");
                s_strcpy(ctx->feed_result.error,
                         e && e->valuestring ? e->valuestring : "Server error",
                         sizeof(ctx->feed_result.error));
                cJSON_Delete(root);
            } else {
                s_strcpy(ctx->feed_result.error, "Server error", sizeof(ctx->feed_result.error));
            }
            http_free_response_body(body);
            ctx->feed_ok = false;
            ctx->feed_done = true;
            rss_blacklist_add(ctx, ctx->feed_channel_id);
            return;
        }

        /* Parse server JSON:
         * { "title":..., "total": N, "episodes":[...] }
         * "total" is the full RSS episode count; "episodes" may be a page. */
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *ttl = cJSON_GetObjectItem(root, "title");
            ESP_LOGI(TAG, "RSS title: %s", ttl && ttl->valuestring ? ttl->valuestring : "(none)");
            cJSON *eps = cJSON_GetObjectItem(root, "episodes");
            int n = eps ? cJSON_GetArraySize(eps) : 0;
            if (n > RSS_MAX_EPISODES) n = RSS_MAX_EPISODES;
            ctx->feed_result.episode_count = n;
            for (int i = 0; i < n; i++) {
                cJSON *ep = cJSON_GetArrayItem(eps, i);
                rss_episode_t *re = &ctx->feed_result.episodes[i];
                js_str(ep, "title", re->title, sizeof(re->title));
                js_str(ep, "audio_url", re->audio_url, sizeof(re->audio_url));
                js_str(ep, "published", re->pub_date, sizeof(re->pub_date));
                js_str(ep, "duration", re->duration, sizeof(re->duration));
            }
            ctx->feed_result.episode_count = n;

            /* Use server-provided total for pagination, fall back to
             * episode_count if the server doesn't include it. */
            cJSON *total_json = cJSON_GetObjectItem(root, "total");
            if (total_json && total_json->type == cJSON_Number)
                ctx->feed_result.total_episodes = (int)total_json->valuedouble;
            else
                ctx->feed_result.total_episodes = n;

            ctx->feed_ok = true;
            cJSON_Delete(root);
        } else {
            ctx->feed_ok = false;
        }
        http_free_response_body(body);
        ctx->feed_done = true;
    }
}

/* ── Download worker ────────────────────────────────────────────────────── */

#define DL_BASE_PATH    "/sdcard/.nomadcast/downloads"
#define DL_MAX_ATTEMPTS 3

/* NOTE: the download worker (CPU1) reads/writes model->download_tasks directly —
 * download_tasks (loaded from the task_store files) is the single source of
 * truth, so no separate worker queue is kept. The worker only touches task
 * fields by task_id and copies url/path to local scratch before the (lock-free)
 * transfer, so structural mutations on CPU0 (create/delete) don't corrupt an
 * in-flight download. This matches the codebase's existing lock-free stance. */

/* Find model array index by persistent task_id. Returns -1 if not found. */
static int dl_find_index(PodcastModel *m, int task_id) {
    for (int i = 0; i < m->download_task_count; i++)
        if (m->download_tasks[i].id == task_id) return i;
    return -1;
}

static bool dl_set_status(PodcastApp *app, int task_id, download_status_t status, int progress) {
    if (!app || !app->model) return false;
    podcast_model_download_lock(app);
    int idx = dl_find_index(app->model, task_id);
    if (idx >= 0) { app->model->download_tasks[idx].status = status; app->model->download_tasks[idx].progress = progress; }
    podcast_model_download_unlock(app);
    return idx >= 0;
}

/* Callback context — set by the worker before http_download_to_file.
 * Must be static: dl_progress_cb signature is fixed by hal.h. */
static DownloadCtx *g_dl_cb_ctx = NULL;

static bool dl_progress_cb(int bytes_done, int total_bytes, int speed_bps)
{
    DownloadCtx *dl = g_dl_cb_ctx;
    if (!dl) return true;
    /* Called every 8 KB chunk now (http_download_to_file checks cancel each
     * chunk). speed_bps is only valid at the 500ms window boundary — keep the
     * last real reading so the UI rate doesn't flicker to 0 in between. */
    if (speed_bps > 0) {
        dl->speed_bps = speed_bps;
        dl->avg_bps = dl->avg_bps > 0 ? (dl->avg_bps * 3 + speed_bps) / 4
                                      : speed_bps;
    }

    if (total_bytes > 0) {
        int rem = total_bytes - bytes_done;
        dl->cur_remaining = rem > 0 ? rem : 0;

        PodcastModel *m = g_podcast_app.model;
        podcast_model_download_lock(&g_podcast_app);
        int idx = m ? dl_find_index(m, dl->current_task_id) : -1;
        if (idx >= 0) {
            DownloadTask *t = &m->download_tasks[idx];
            /* Live % — RAM only; persisted at completion, not every 500ms tick
             * (an SD write per tick would thrash the card). */
            t->progress = (int)((int64_t)bytes_done * 100 / total_bytes);
            /* Learn bytes-per-audio-second to size queued (not-yet-open) tasks. */
            if (t->duration_sec > 0) {
                int r = total_bytes / t->duration_sec;
                dl->bytes_per_audio_sec = dl->bytes_per_audio_sec > 0
                    ? (dl->bytes_per_audio_sec * 3 + r) / 4 : r;
            }
        }
        podcast_model_download_unlock(&g_podcast_app);
    } else {
        dl->cur_remaining = 0;   /* chunked / size unknown → ETA falls back to estimate */
    }

    if (!dl->running || dl->abort_current) return false;   /* stop/cancel */
    if (dl->user_paused)   return false;   /* user pause — abort the in-flight transfer */
    if (audio_player_is_playing()) {        /* playback started → yield */
        dl->yield_to_audio = true;
        return false;                       /* http_download_to_file aborts + unlinks partial */
    }
    return true;
}

/* Pick the next PENDING task: copy the fields the transfer needs into caller
 * buffers and mark it DOWNLOADING. Returns the task_id, or -1 if none pending. */
static int dl_take_next_pending(PodcastApp *app, char *url, int url_sz,
                                char *path, int path_sz)
{
    PodcastModel *m = app->model;
    if (!m) return -1;
    podcast_model_download_lock(app);
    for (int i = 0; i < m->download_task_count; i++) {
        DownloadTask *t = &m->download_tasks[i];
        if (t->status != DOWNLOAD_STATUS_PENDING) continue;
        if (!t->audio_url[0] || !t->file_path[0]) continue;
        snprintf(url,  url_sz,  "%s", t->audio_url);
        snprintf(path, path_sz, "%s", t->file_path);
        int id = t->id;
        t->status   = DOWNLOAD_STATUS_DOWNLOADING;
        t->progress = 0;
        podcast_model_download_unlock(app);
        task_store_update(app, id);
        return id;
    }
    podcast_model_download_unlock(app);
    return -1;
}

/* Producer side of the completion ring (worker/CPU1). */
static void dl_done_push(PodcastCtrlCtx *ctx, int task_id)
{
    if (!ctx) return;
    int nh = (ctx->dl.done_head + 1) % DL_DONE_RING;
    if (nh == ctx->dl.done_tail) return;
    ctx->dl.done_ids[ctx->dl.done_head] = task_id;
    ctx->dl.done_head = nh;
}

static void dl_worker_task(void *arg)
{
    PodcastApp *app = (PodcastApp *)arg;
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) { vTaskDelete(NULL); return; }
    /* Static scratch — only the worker touches these, one transfer at a time.
     * Kept off the 10KB stack (url 1KB + path 0.5KB). */
    static char s_url[1024];
    static char s_path[512];

    while (ctx->dl.running) {
        xSemaphoreTake(ctx->dl.sem, pdMS_TO_TICKS(5000));

        /* Drain all PENDING tasks this wake (serial — one transfer at a time).
         * dl_take_next_pending re-scans the model each iteration, so a task
         * added mid-drain is picked up here too. */
        while (ctx->dl.running) {
            PodcastApp *app = &g_podcast_app;
            if (!app || !app->model) break;

            /* Priority: Display > Playback > Download.
             * If audio is actively PLAYING, don't steal its memory — wait.
             * If paused, release the pipeline (position saved) to free DRAM. */
            if (audio_player_is_playing()) {
                break;  /* playback is running — retry later */
            }
            if (audio_player_is_active() && !audio_player_is_local_source() &&
                audio_player_memory_pressure()) {
                ESP_LOGW(TAG, "dl: releasing paused audio pipeline under DRAM pressure");
                audio_player_release();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            if (audio_player_is_active() && audio_player_is_local_source()) {
                /* Local M4A cannot be safely reconstructed from an arbitrary
                 * compressed byte offset; keep its decoder alive and defer
                 * downloads until playback is stopped. */
                break;
            }

            /* User-requested pause — wait for resume signal */
            if (ctx->dl.user_paused) {
                xSemaphoreTake(ctx->dl.sem, pdMS_TO_TICKS(2000));
                continue;
            }

            int task_id = dl_take_next_pending(app, s_url, sizeof(s_url),
                                                     s_path, sizeof(s_path));
            if (task_id < 0) break;   /* nothing pending */

            audio_player_log_memory("before-download");
            ESP_LOGI(TAG, "dl: downloading task %d → %s", task_id, s_path);

            ctx->dl.current_task_id = task_id;
            ctx->dl.abort_current   = false;
            ctx->dl.yield_to_audio  = false;
            ctx->dl.active          = true;
            /* Retry transient failures (connect abort, dropped socket) a few
             * times before marking FAILED — but stop immediately if the user
             * cancelled (ctx->dl.abort_current) or audio started (ctx->dl.yield_to_audio). */
            bool ok = false;
            g_dl_cb_ctx = &ctx->dl;
            for (int attempt = 1; attempt <= DL_MAX_ATTEMPTS; attempt++) {
                ok = http_download_to_file(s_url, s_path, dl_progress_cb);
                if (ok || ctx->dl.abort_current || ctx->dl.yield_to_audio) break;
                if (attempt < DL_MAX_ATTEMPTS) {
                    ESP_LOGW(TAG, "dl: task %d attempt %d/%d failed — retrying",
                             task_id, attempt, DL_MAX_ATTEMPTS);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
            g_dl_cb_ctx             = NULL;
            ctx->dl.active          = false;
            ctx->dl.current_task_id = -1;
            ctx->dl.speed_bps       = 0;
            ctx->dl.cur_remaining   = 0;

            /* Re-find by id — the task may have been cancelled/deleted while the
             * transfer ran (its model entry is gone). */
            podcast_model_download_lock(app);
            int idx = dl_find_index(app->model, task_id);
            podcast_model_download_unlock(app);
            if (idx < 0) {
                /* Cancelled mid-flight: on abort http already unlinked the
                 * partial; if it finished before the cancel landed, remove the
                 * now-orphaned file. */
                remove(s_path);
                ESP_LOGI(TAG, "dl: task %d cancelled — file removed", task_id);
                continue;
            }
            if (ok) {
                dl_set_status(app, task_id, DOWNLOAD_STATUS_COMPLETED, 100);
                task_store_update(app, task_id);
                dl_done_push(ctx, task_id);   /* hand off to CPU0 for event + cache */
                ESP_LOGI(TAG, "dl: task %d OK", task_id);
            } else if (ctx->dl.yield_to_audio) {
                /* Paused for playback: keep PENDING (in RAM only — avoid an SD
                 * write while audio holds the memory) and stop draining. Resumes
                 * on the next re-scan after playback ends; on reboot dl_resume
                 * turns the persisted DOWNLOADING back into PENDING. Re-downloads
                 * from the start (the partial was unlinked on abort). */
                dl_set_status(app, task_id, DOWNLOAD_STATUS_PENDING, 0);
                ESP_LOGI(TAG, "dl: task %d paused for playback (will resume)", task_id);
                break;
            } else if (ctx->dl.user_paused) {
                /* User pressed Pause: keep the task PENDING and stop draining.
                 * The partial was unlinked on abort, so resume re-downloads from
                 * the start. RAM-only status — the worker now parks until resume. */
                dl_set_status(app, task_id, DOWNLOAD_STATUS_PENDING, 0);
                ESP_LOGI(TAG, "dl: task %d paused by user (will resume)", task_id);
                break;
            } else {
                dl_set_status(app, task_id, DOWNLOAD_STATUS_FAILED, 0);
                task_store_update(app, task_id);
                ctx->dl.status_dirty = true;   /* nudge CPU0 to refresh the stats card */
                ESP_LOGW(TAG, "dl: task %d FAILED after %d attempts",
                         task_id, DL_MAX_ATTEMPTS);
            }
        }
    }
    ctx->dl.task_handle = NULL;
    ctx->dl.running = false;
    vTaskDelete(NULL);
}

/* ── Resume pending downloads after reboot / WiFi reconnect ────────────── */

void podcast_controller_resume_downloads(struct PodcastApp *app) { dl_resume_pending(app); }

void podcast_controller_pause_all_downloads(struct PodcastApp *app) {
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return;
    ctx->dl.user_paused = true;
    ESP_LOGI(TAG, "dl: user paused downloads");
}

void podcast_controller_resume_all_downloads(struct PodcastApp *app) {
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return;
    ctx->dl.user_paused = false;
    if (ctx->dl.sem) xSemaphoreGive(ctx->dl.sem);
    ESP_LOGI(TAG, "dl: user resumed downloads");
}

bool podcast_controller_is_download_paused(struct PodcastApp *app) {
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    return ctx ? ctx->dl.user_paused : false;
}

static void dl_resume_pending(PodcastApp *app)
{
    if (!app || !app->model) return;
    PodcastModel *m = app->model;
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return;

    /* Ensure the worker is running */
    if (!ctx->dl.running) dl_worker_start(app);

    int pending = 0;
    int reset_ids[64]; int reset_count = 0;
    podcast_model_download_lock(app);
    for (int i = 0; i < m->download_task_count; i++) {
        DownloadTask *t = &m->download_tasks[i];
        if (t->status == DOWNLOAD_STATUS_DOWNLOADING) {
            /* Was mid-download when power cut — reset to PENDING */
            t->status = DOWNLOAD_STATUS_PENDING;
            t->progress = 0;
            if (reset_count < 64) reset_ids[reset_count++] = t->id;
        }
        if (t->status == DOWNLOAD_STATUS_PENDING &&
            t->audio_url[0] && t->file_path[0])
            pending++;
    }
    podcast_model_download_unlock(app);
    for (int i = 0; i < reset_count; i++) task_store_update(app, reset_ids[i]);

    /* One wake is enough — the worker drains every PENDING task it finds. */
    if (pending > 0 && ctx->dl.sem) {
        xSemaphoreGive(ctx->dl.sem);
        ESP_LOGI(TAG, "dl_resume: %d pending task(s), worker woken", pending);
    }
}

static void dl_worker_start(PodcastApp *app)
{
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return;
    if (ctx->dl.running) return;
    ctx->dl.running = true;
    ctx->dl.sem = xSemaphoreCreateCounting(64, 0);
    if (!ctx->dl.sem) { ctx->dl.running = false; ESP_LOGE(TAG, "download semaphore OOM"); return; }
    if (xTaskCreatePinnedToCore(dl_worker_task, "podcast_dl", 10240, app,
                            1, &ctx->dl.task_handle, 1) != pdPASS) {
        vSemaphoreDelete(ctx->dl.sem); ctx->dl.sem = NULL; ctx->dl.running = false;
        ESP_LOGE(TAG, "download worker create failed");
    }
}

/* ── ETA telemetry getters (read by the download-task page on the LVGL thread) ── */
int podcast_dl_avg_speed_bps(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->dl.avg_bps : 0;
}
int podcast_dl_current_task_id(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->dl.current_task_id : -1;
}
int podcast_dl_current_remaining_bytes(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->dl.cur_remaining : 0;
}
int podcast_dl_bytes_per_audio_sec(void) {
    PodcastCtrlCtx *ctx = ctl_ctx(&g_podcast_app);
    return ctx ? ctx->dl.bytes_per_audio_sec : 0;
}

void controller_process_download(void)
{
    PodcastApp *app = &g_podcast_app;
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!app || !app->model || !ctx) return;

    /* Push live download state to the global status bar (LVGL thread) */
    lv_status_bar_t *sb = lv_status_bar_get();
    if (sb) {
        bool active = ctx->dl.active;
        lv_status_bar_set_download_speed(sb, active ? ctx->dl.speed_bps : -1);
        lv_status_bar_set_download_active(sb, active);
    }

    /* Drain completed-download notifications from the worker: the worker already
     * set status=COMPLETED; here (on the LVGL thread) we fire the UI event and
     * add the finished episode to the local cache. Looked up by task_id from the
     * model (the single source of truth). */
    PodcastModel *m = app->model;
    while (ctx->dl.done_tail != ctx->dl.done_head) {
        int task_id = ctx->dl.done_ids[ctx->dl.done_tail];
        ctx->dl.done_tail = (ctx->dl.done_tail + 1) % DL_DONE_RING;

        int idx = dl_find_index(m, task_id);
        if (idx < 0) continue;   /* deleted before we processed the notification */
        DownloadTask *t = &m->download_tasks[idx];
        cache_local_add(app, t->channel_id, t->channel_title,
                        t->episode_id, t->episode_title, t->audio_url,
                        t->duration_sec, t->file_path, t->collection_id);
        ESP_LOGI(TAG, "process_dl: firing DOWNLOAD_COMPLETED (task %d)", task_id);
        app_event_fire(APP_EVENT_DOWNLOAD_COMPLETED, NULL);
    }

    /* A failure (set by the worker) never reaches the done-ring, so refresh the
     * UI here so the stats card leaves its stale "downloading" snapshot. */
    if (ctx->dl.status_dirty) {
        ctx->dl.status_dirty = false;
        app_event_fire(APP_EVENT_DOWNLOAD_CHANGED, NULL);
    }
}

/* ── Cancel / delete download tasks ─────────────────────────────────────── */

/* Cancel pending/downloading tasks: abort the in-flight transfer (the worker
 * unlinks its own partial file), drop still-queued items, remove any leftover
 * partial file, and delete the persisted + in-memory record. Completed/failed
 * ids are ignored here. */
void podcast_controller_cancel_download_tasks(struct PodcastApp *app, const int *ids, int n)
{
    if (!app || !app->model || n <= 0) return;
    PodcastModel *m = app->model;
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return;

    for (int k = 0; k < n; k++) {
        int tid = ids[k];
        int idx = dl_find_index(m, tid);
        if (idx < 0) continue;

        DownloadTask *t = &m->download_tasks[idx];
        download_status_t s = t->status;
        if (s != DOWNLOAD_STATUS_PENDING && s != DOWNLOAD_STATUS_DOWNLOADING) continue;

        if (ctx->dl.active && ctx->dl.current_task_id == tid) {
            /* In-flight: signal the worker to abort. It stops the transfer,
             * unlinks its own partial file, and (finding the record gone) skips
             * the status write. */
            ctx->dl.abort_current = true;
            ESP_LOGI(TAG, "cancel: aborting active download task %d", tid);
        } else {
            /* PENDING (not started yet): remove any partial file left on disk. */
            if (t->file_path[0]) remove(t->file_path);
        }

        /* Delete persisted JSON + in-memory record (won't resurrect on reboot). */
        task_store_delete(app, tid);
    }
}

/* Remove completed/failed task records: delete the persisted JSON + in-memory
 * record but KEEP the downloaded audio file on disk. */
void podcast_controller_delete_download_records(struct PodcastApp *app, const int *ids, int n)
{
    if (!app || !app->model || n <= 0) return;
    for (int k = 0; k < n; k++)
        task_store_delete(app, ids[k]);  /* unlinks only the JSON, not the audio */
}

/* ── Play ─────────────────────────────────────────────────────────────── */

static void podcast_proxy_url(char *out, int out_sz, const char *url,
                               const char *endpoint);

static bool podcast_is_m4a_url(const char *url)
{
    if (!url) return false;
    const char *q = strpbrk(url, "?#");
    size_t n = q ? (size_t)(q - url) : strlen(url);
    if (n < 4) return false;
    const char *ext = url + n - 4;
    return strncasecmp(ext, ".m4a", 4) == 0 ||
           (n >= 4 && strncasecmp(ext, ".m4b", 4) == 0);
}

void podcast_media_url(char *out, int out_sz, const char *url)
{
    if (!out || out_sz < 1) return;
    out[0] = '\0';
    if (!url || !url[0]) return;

    bool remote  = (strncmp(url, "http://", 7) == 0 ||
                    strncmp(url, "https://", 8) == 0);
    bool proxied = (strncmp(url, PODCAST_SERVER, strlen(PODCAST_SERVER)) == 0);

    /* Local file (/sdcard/...) or already-proxied URL → use verbatim */
    if (!remote || proxied) {
        snprintf(out, out_sz, "%s", url);
        return;
    }

    /* Prefer direct CDN playback for formats the ADF reader can seek safely.
     * Remote M4A is handled by podcast_controller_play_episode(): users are
     * asked to download it first so playback uses the local seekable file. */
    snprintf(out, out_sz, "%s", url);
}

void podcast_download_url(char *out, int out_sz, const char *url)
{
    if (!out || out_sz < 1) { out[0] = '\0'; return; }
    out[0] = '\0';
    if (!url || !url[0]) return;

    bool remote  = (strncmp(url, "http://", 7) == 0 ||
                    strncmp(url, "https://", 8) == 0);
    bool proxied = (strncmp(url, PODCAST_SERVER, strlen(PODCAST_SERVER)) == 0);

    if (!remote || proxied) {
        snprintf(out, out_sz, "%s", url);
        return;
    }

    /* Use the server's byte-forwarding proxy for SD downloads.  It follows
     * the publisher's HTTPS redirects and preserves Range requests, while
     * the ESP32 only maintains a plain LAN connection.  No transcoding or
     * server-side audio buffering is involved. */
    podcast_proxy_url(out, out_sz, url, "/api/raw");
}

static void podcast_proxy_url(char *out, int out_sz, const char *url,
                               const char *endpoint)
{
    /* Build: PODCAST_SERVER/endpoint?url=<encoded> */
    char *w = out;
    const char *prefix = PODCAST_SERVER;
    while (*prefix && (w - out) < out_sz - 1) *w++ = *prefix++;
    while (*endpoint && (w - out) < out_sz - 1) *w++ = *endpoint++;
    const char *qs = "?url=";
    while (*qs && (w - out) < out_sz - 1) *w++ = *qs++;

    for (const char *p = url; *p && (w - out) < out_sz - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' || c == '#' || c == '?' || c == '&' || c == ' ' || c == '+') {
            w += snprintf(w, 4, "%%%02X", c);
        } else {
            *w++ = c;
        }
    }
    *w = '\0';
}

/* Build the on-SD path a downloaded episode is stored at. Shared by the
 * downloader (file naming) and the player (lookup) so they can never diverge. */
static void podcast_local_audio_path(const char *ch_title, const char *ep_title,
                                     char *out, int out_sz)
{
    char ch_safe[256], ep_safe[512];
    snprintf(ch_safe, sizeof(ch_safe), "%s", ch_title ? ch_title : "unknown");
    snprintf(ep_safe, sizeof(ep_safe), "%s.m4a", ep_title ? ep_title : "audio");
    for (char *p = ch_safe; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    for (char *p = ep_safe; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    snprintf(out, out_sz, DL_BASE_PATH "/%s/%s", ch_safe, ep_safe);
}

void podcast_controller_media_for_episode(struct PodcastApp *app, int eid,
                                          char *out, int out_sz)
{
    if (!out || out_sz < 1) return;
    out[0] = '\0';
    const Episode *ep = podcast_model_get_episode_by_id(app, eid);
    if (!ep) return;

    /* Prefer the downloaded local file if present on SD (plays offline). */
    const Channel *ch = podcast_model_get_channel_by_id(app, ep->channel_id);
    if (ch) {
        char local[1536];
        podcast_local_audio_path(ch->title, ep->title, local, sizeof(local));
        FILE *f = fopen(local, "rb");
        if (f) {
            fclose(f);
            snprintf(out, out_sz, "%s", local);
            ESP_LOGI(TAG, "media: local file %s", local);
            return;
        }
    }

    /* Fall back to the network/proxy stream. */
    podcast_media_url(out, out_sz, ep->audio_url);
}

bool podcast_controller_episode_playable(struct PodcastApp *app, int eid)
{
    /* Downloaded local file → plays offline regardless of format. */
    char media[2560];
    podcast_controller_media_for_episode(app, eid, media, sizeof(media));
    if (strncmp(media, "/sdcard/", 8) == 0) return true;

    /* Remote stream: M4A can't be decoded without server transcoding (disabled),
     * so it must be downloaded first. Other formats keep the old stream path. */
    const Episode *ep = podcast_model_get_episode_by_id(app, eid);
    return ep ? !podcast_is_m4a_url(ep->audio_url) : false;
}

void podcast_controller_toggle_play_pause(struct PodcastApp *app)
{
    if (!app) return;
    int eid = podcast_model_get_current_episode_id(app);
    if (eid <= 0) return;                    /* nothing loaded */

    if (podcast_model_is_playing(app)) {
        hal_audio_pause(true);
        podcast_model_set_playing(app, false);
        ESP_LOGI(TAG, "toggle: pause (eid=%d)", eid);
    } else {
        /* Resume the paused pipeline in place — position preserved.
         * (audio_player keeps the pipeline alive across an intentional pause.) */
        hal_audio_pause(false);
        podcast_model_set_playing(app, true);
        ESP_LOGI(TAG, "toggle: resume (eid=%d)", eid);
    }
}

void podcast_controller_play_episode(struct PodcastApp *app, int eid) {
    const Episode *ep = podcast_model_get_episode_by_id(app, eid);
    if (!ep || !ep->audio_url[0]) return;

    char media[2560];
    podcast_controller_media_for_episode(app, eid, media, sizeof(media));
    if (!media[0]) return;

    if (strncmp(media, "/sdcard/", 8) != 0 && podcast_is_m4a_url(ep->audio_url)) {
        lv_toast_show("M4A 请先下载后播放", 3000);
        ESP_LOGI(TAG, "play: remote M4A requires download before playback");
        podcast_model_set_playing(app, false);
        return;
    }

    int ids[1] = {eid}; podcast_model_set_queue(app, ids, 1);

    /* A local file is played as-is — never fall back to the network. If it's
     * empty/missing (e.g. a download that failed), surface a clear error instead
     * of a cryptic decoder failure. */
    if (strncmp(media, "/sdcard/", 8) == 0) {
        struct stat st;
        if (stat(media, &st) != 0 || st.st_size == 0) {
            ESP_LOGW(TAG, "play: local file empty/missing: %s", media);
            lv_toast_show("This download is empty or corrupt", 2500);
            podcast_model_set_playing(app, false);
            return;
        }
    }

    podcast_model_set_playing(app, true);
    hal_audio_play_file(media);
}

void podcast_controller_play_channel(struct PodcastApp *app, int cid) {
    int n; const Episode **eps = podcast_model_get_episodes_by_channel(app, cid, &n);
    if (!eps || !n) return;
    int *ids = (int *)malloc(sizeof(int) * n);
    for (int i=0;i<n;i++) ids[i]=eps[i]->id;
    podcast_model_set_queue(app, ids, n); free(ids); free((void*)eps);
    podcast_model_set_playing(app, true);
    podcast_controller_play_episode(app, podcast_model_queue_current(app));
}

/* ── Download API ─────────────────────────────────────────────────────── */

bool podcast_controller_download_episode(struct PodcastApp *app, const char *url,
    const char *title, const char *channel_title)
{ return podcast_controller_download_episode_ex2(app, url, title, channel_title, 0, 0, 0, 0); }

bool podcast_controller_download_episode_ex(struct PodcastApp *app, const char *url,
    const char *title, const char *channel_title, int ch_id, int ep_id)
{ return podcast_controller_download_episode_ex2(app, url, title, channel_title, ch_id, ep_id, 0, 0); }

bool podcast_controller_download_episode_ex2(struct PodcastApp *app, const char *url,
    const char *title, const char *channel_title, int ch_id, int ep_id, int dur, int col_id)
{
    if (!url || !url[0] || !title) return false;
    PodcastCtrlCtx *ctx = ctl_ctx(app);
    if (!ctx) return false;

    /* Build the SD destination path (shared with playback lookup). */
    char path[1536];
    podcast_local_audio_path(channel_title, title, path, sizeof(path));

    /* Ensure download worker is running */
    if (!ctx->dl.running) dl_worker_start(app);

    /* Route audio download through the proxy server. */
    char proxy_url[2560];
    podcast_download_url(proxy_url, sizeof(proxy_url), url);

    /* Persist to SD + model array — file-backed, survives reboot. The task is
     * now a PENDING entry in download_tasks (the worker's source of truth). */
    int task_id = task_store_create(app, title, channel_title,
                                     dur, path, proxy_url,
                                     ep_id, ch_id, col_id);
    if (task_id < 0) {
        ESP_LOGE(TAG, "dl: task_store_create failed");
        return false;
    }

    /* Wake the worker — it scans download_tasks for PENDING and downloads. */
    if (ctx->dl.sem) xSemaphoreGive(ctx->dl.sem);
    ESP_LOGI(TAG, "dl: task %d created '%s' → %s", task_id, title, path);
    return true;
}

/* ── Login ───────────────────────────────────────────────────────────── */

bool podcast_controller_login(struct PodcastApp *app, const char *n, const char *p,
    const char *cp, bool registering, bool ag, const char **err) {
    if (!n||strlen(n)<2) {*err="Name too short";return false;}
    if (!p||strlen(p)<4) {*err="Password too short";return false;}
    if (registering && (!cp || strcmp(p,cp))) {*err="Mismatch";return false;}
    if (!ag) {*err="Agree";return false;}

    /* Determine endpoint: register if passwords differ (confirm-password mode),
     * otherwise login. */
    const char *endpoint = registering ? "/api/register" : "/api/login";

    /* Build JSON body */
    const char *dev_id = podcast_model_get_device_id(app);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"name\":\"%s\",\"password\":\"%s\",\"device_id\":\"%s\"}",
             n, p, dev_id ? dev_id : "");

    /* Call server */
    char url[512];
    snprintf(url, sizeof(url), "%s%s", PODCAST_SERVER, endpoint);
    ESP_LOGI(TAG, "POST %s…", url);

    int status = 0, len = 0;
    char *resp = http_post_json_sync(url, json, &status, &len);

    if (!resp || status == 0) {
        *err = "Server unreachable";
        ESP_LOGW(TAG, "Server unreachable");
        if (resp) http_free_response_body(resp);
        return false;
    }

    if (status != 200 && status != 201) {
        /* Try to extract error message from JSON response */
        cJSON *root = cJSON_Parse(resp);
        const char *msg = "Server error";
        static char err_buf[128];
        if (root) {
            cJSON *e = cJSON_GetObjectItem(root, "error");
            if (e && e->valuestring) { s_strcpy(err_buf, e->valuestring, sizeof(err_buf)); msg = err_buf; }
            cJSON_Delete(root);
        }
        *err = msg;
        ESP_LOGW(TAG, "Server returned %d: %s", status, msg);
        http_free_response_body(resp);
        return false;
    }

    /* Parse user_id from response */
    cJSON *root = cJSON_Parse(resp);
    const char *uid = NULL;
    if (root) {
        cJSON *id = cJSON_GetObjectItem(root, "user_id");
        if (id && id->valuestring) uid = id->valuestring;
    }

    if (!uid) {
        *err = "Invalid server response";
        ESP_LOGW(TAG, "No user_id in response");
        if (root) cJSON_Delete(root);
        http_free_response_body(resp);
        return false;
    }

    podcast_model_set_login(app, n, p, uid);
    ESP_LOGI(TAG, "%s OK — user ID: %s",
             (cp != p) ? "Register" : "Login", uid);

    if (root) cJSON_Delete(root);
    http_free_response_body(resp);
    return true;
}

/* ── Local content ───────────────────────────────────────────────────── */

/* SD-mount check — same authoritative source as the Settings storage page:
 * FatFS drive "0:" is the mounted SD volume (/sdcard). f_getfree() succeeds
 * only when a card is actually mounted. */
int hal_sdcard_is_mounted(void) {
    FATFS *fs = NULL;
    DWORD free_clst = 0;
    return f_getfree("0:/", &free_clst, &fs) == FR_OK && fs != NULL;
}

local_content_state_t podcast_controller_check_local_content(struct PodcastApp *app) {
    bool sd = hal_sdcard_is_mounted(); int t = 0;
    for (int c = 0; c < CHANNEL_CATEGORY_COUNT; c++) {
        int n = 0; const Channel **p = podcast_model_get_downloaded_channels_by_category(app, (channel_category_t)c, &n);
        t += n; if (p) free((void *)p);
    }
    podcast_model_set_local_state(app, sd, t > 0);
    return sd ? (t > 0 ? LOCAL_HAS_CONTENT : LOCAL_NO_CONTENT) : LOCAL_SD_MISSING;
}

void podcast_controller_poll(struct PodcastApp *a) { (void)a; }

/* Delete a channel's local audio files on SD card + metadata.
 * Called from the Local page when user left-swipes a channel and confirms. */
void podcast_controller_delete_channel_local(struct PodcastApp *app, int channel_id)
{
    if (!app || !app->model) return;
    PodcastModel *m = app->model;

    /* 1. Find the channel */
    const Channel *ch = NULL;
    for (int i = 0; i < m->local_channel_count; i++) {
        if (m->local_channels[i].id == channel_id) {
            ch = &m->local_channels[i];
            break;
        }
    }
    if (!ch) return;

    printf("[INF] delete_channel_local: '%s' (id=%d)\n", ch->title, channel_id);
    fflush(stdout);

    /* 2. Delete each episode's audio file + count them */
    int deleted_files = 0;
    for (int i = 0; i < m->local_episode_count; i++) {
        if (m->local_episodes[i].channel_id != channel_id) continue;

        char audio_path[2048];
        podcast_local_audio_path(ch->title, m->local_episodes[i].title,
                                 audio_path, sizeof(audio_path));
        if (remove(audio_path) == 0) {
            printf("[INF] Deleted: %s\n", audio_path); fflush(stdout);
            deleted_files++;
        }
    }

    /* 3. Remove the channel directory (may fail if not empty — ignore) */
    {
        char ch_dir[1536];
        char ch_safe[256];
        snprintf(ch_safe, sizeof(ch_safe), "%s", ch->title);
        for (char *p = ch_safe; *p; p++)
            if (strchr("\\/:*?\"<>|", *p)) *p = '_';
        snprintf(ch_dir, sizeof(ch_dir), DL_BASE_PATH "/%s", ch_safe);
        remove(ch_dir);   /* best-effort; FATFS remove fails if dir not empty */
    }

    /* 4. Delete the bucket metadata file */
    {
        char meta[320];
        snprintf(meta, sizeof(meta),
                 DL_BASE_PATH "/.meta/%d.json",
                 ch->collection_id > 0 ? ch->collection_id : channel_id);
        remove(meta);
    }

    /* 5. Remove episodes from model */
    {
        int write = 0;
        for (int i = 0; i < m->local_episode_count; i++) {
            if (m->local_episodes[i].channel_id != channel_id) {
                if (write != i) m->local_episodes[write] = m->local_episodes[i];
                write++;
            }
        }
        m->local_episode_count = write;
        if (write > 0) {
            Episode *ne = realloc(m->local_episodes, write * sizeof(Episode));
            if (ne) m->local_episodes = ne;
        } else {
            free(m->local_episodes);
            m->local_episodes = NULL;
        }
    }

    /* 6. Remove channel from model */
    {
        int write = 0;
        for (int i = 0; i < m->local_channel_count; i++) {
            if (m->local_channels[i].id != channel_id) {
                if (write != i) m->local_channels[write] = m->local_channels[i];
                write++;
            }
        }
        m->local_channel_count = write;
        if (write > 0) {
            Channel *nc = realloc(m->local_channels, write * sizeof(Channel));
            if (nc) m->local_channels = nc;
        } else {
            free(m->local_channels);
            m->local_channels = NULL;
        }
    }

    /* 7. Update has_content flag */
    m->local_has_content = (m->local_channel_count > 0);

    printf("[INF] delete_channel_local: %d files deleted, done\n", deleted_files);
    fflush(stdout);
}
