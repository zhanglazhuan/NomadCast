/**
 * @file controller.c — Podcast controller (ESP32 only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "i_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ff.h"      /* FatFS f_getfree — SD-mount check */
#include "cJSON.h"

static const char *TAG = "podcast_ctrl";

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

static char      g_feed_url[2048];
static rss_feed_t g_feed_result;
static bool       g_feed_pending, g_feed_done, g_feed_ok;
static int        g_feed_channel_id = 0;

#define RSS_BLACKLIST_MAX 32
static int g_rss_blacklist[RSS_BLACKLIST_MAX];
static int g_rss_blacklist_count = 0;

static bool rss_is_blacklisted(int channel_id) {
    for (int i = 0; i < g_rss_blacklist_count; i++)
        if (g_rss_blacklist[i] == channel_id) return true;
    return false;
}

static void rss_blacklist_add(int channel_id) {
    if (rss_is_blacklisted(channel_id)) return;
    if (g_rss_blacklist_count >= RSS_BLACKLIST_MAX) return;
    g_rss_blacklist[g_rss_blacklist_count++] = channel_id;
    ESP_LOGW(TAG, "Channel %d blacklisted (RSS unreachable)", channel_id);
}

bool g_rss_done(void) { return g_feed_done; }
bool g_rss_ok(void)   { return g_feed_ok; }
rss_feed_t *g_rss_result(void) { return &g_feed_result; }
int g_rss_channel_id(void) { return g_feed_channel_id; }

void controller_rss_reparse(int offset, int limit) {
    /* On ESP32, all episodes are imported from the proxy server JSON
     * in controller_process_rss().  Re-parsing is a no-op — the view
     * reads from current_channel_episodes in the model directly. */
    (void)offset; (void)limit;
    g_feed_done = true;
    g_feed_ok = true;
}

/* ── WiFi event listener — updates podcast model net_state ──────────── */

extern PodcastApp g_podcast_app;

static void dl_resume_pending(PodcastApp *app);
static void dl_worker_start(void);

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

void podcast_controller_init(struct PodcastApp *app) {
    /* Force cJSON to use PSRAM for all allocations.  Chart JSON is ~25KB
     * and creates 400+ small nodes (~22KB).  Default malloc routes small
     * allocations to internal DRAM (~50KB total) → OOM → parse fail. */
    static cJSON_Hooks psram_hooks = {
        .malloc_fn = (void *(*)(size_t))psram_malloc,
        .free_fn   = free,
    };
    cJSON_InitHooks(&psram_hooks);

    app->controller = (PodcastController *)calloc(1, sizeof(PodcastController));
    backend_init();
    app_event_register(on_wifi_event);
}

void podcast_controller_deinit(struct PodcastApp *app) {
    if (app->controller) {
        backend_deinit();
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

bool podcast_controller_fetch_chart(struct PodcastApp *app)
{
    bk_channel_t *bk = NULL;
    int count = 0;

    char cc[8]; podcast_country(cc, sizeof(cc));

    /* Single attempt — retrying a dead server wastes 15+ seconds each time */
    count = backend_fetch_chart(&bk, cc, 50);

    if (count <= 0) {
        podcast_model_set_net_state(app, NET_STATE_ERROR,
            hal_wifi_is_connected() ? "Server unreachable" : "No network connection");
        ESP_LOGW(TAG, "fetch_chart: 0 channels");
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
    const Channel *ch = podcast_model_get_channel_by_id(app, cid);
    if (!ch) return false;

    /* Blacklist: skip HTTP fetch for channels that previously returned 502/unreachable */
    if (rss_is_blacklisted(cid)) {
        memset(&g_feed_result, 0, sizeof(g_feed_result));
        s_strcpy(g_feed_result.error, "Feed unavailable (blacklisted)",
                 sizeof(g_feed_result.error));
        g_feed_done = true;
        g_feed_ok = false;
        g_feed_channel_id = cid;
        ESP_LOGI(TAG, "Channel %d blacklisted — skipping fetch", cid);
        return true;
    }

    memset(&g_feed_result, 0, sizeof(g_feed_result));
    g_feed_done = g_feed_ok = false;
    g_feed_channel_id = cid;
    if (ch->feed_url[0]) {
        snprintf(g_feed_url, sizeof(g_feed_url), "%s", ch->feed_url);
    } else {
        snprintf(g_feed_url, sizeof(g_feed_url), "SEARCH:%s", ch->title);
    }
    g_feed_pending = true;
    return true;
}

void controller_process_rss(void) {
    if (!g_feed_pending) return;
    g_feed_pending = false;

    if (strncmp(g_feed_url, "SEARCH:", 7) == 0) {
        char cc[8]; podcast_country(cc, sizeof(cc));
        bk_channel_t *r = NULL; int n = backend_search_podcasts_sync(&r, g_feed_url + 7, cc, 3);
        if (r && n > 0) {
            for (int i = 0; i < n; i++) {
                if (r[i].feed_url[0]) {
                    snprintf(g_feed_url, sizeof(g_feed_url), "%s", r[i].feed_url);
                    free(r); g_feed_pending = true; return;
                }
            }
            free(r);
        }
        g_feed_done = true; g_feed_ok = false;
        rss_blacklist_add(g_feed_channel_id);
        return;
    }

    /*
     * Route RSS fetch through the podcast proxy server.
     * Server does HTTPS + XML parse → returns clean JSON.
     * ESP32 only does one HTTP call and parses simple JSON.
     */
    {
        char url[2560];
        const Channel *ch = podcast_model_get_channel_by_id(&g_podcast_app, g_feed_channel_id);
        int col_id = ch ? ch->collection_id : 0;
        snprintf(url, sizeof(url),
                 "%s/api/episodes?feed_url=%s&collection_id=%d",
                 PODCAST_SERVER, g_feed_url, col_id);
        int st, len;
        char *body = http_get_sync(url, &st, &len);
        memset(&g_feed_result, 0, sizeof(g_feed_result));

        if (!body) {
            const char *detail = http_last_error();
            s_strcpy(g_feed_result.error,
                     detail && detail[0] ? detail : "Server unreachable",
                     sizeof(g_feed_result.error));
            g_feed_ok = false;
            g_feed_done = true;
            rss_blacklist_add(g_feed_channel_id);
            return;
        }

        ESP_LOGI(TAG, "RSS proxy: HTTP %d, %d bytes", st, len);

        /* Check for server error — response may contain {"error": "..."} */
        if (st != 200) {
            cJSON *root = cJSON_Parse(body);
            if (root) {
                cJSON *e = cJSON_GetObjectItem(root, "error");
                s_strcpy(g_feed_result.error,
                         e && e->valuestring ? e->valuestring : "Server error",
                         sizeof(g_feed_result.error));
                cJSON_Delete(root);
            } else {
                s_strcpy(g_feed_result.error, "Server error", sizeof(g_feed_result.error));
            }
            http_free_response_body(body);
            g_feed_ok = false;
            g_feed_done = true;
            rss_blacklist_add(g_feed_channel_id);
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
            g_feed_result.episode_count = n;
            for (int i = 0; i < n; i++) {
                cJSON *ep = cJSON_GetArrayItem(eps, i);
                rss_episode_t *re = &g_feed_result.episodes[i];
                js_str(ep, "title", re->title, sizeof(re->title));
                js_str(ep, "audio_url", re->audio_url, sizeof(re->audio_url));
                js_str(ep, "published", re->pub_date, sizeof(re->pub_date));
                js_str(ep, "duration", re->duration, sizeof(re->duration));
                js_str(ep, "description", re->description, sizeof(re->description));
            }
            g_feed_result.episode_count = n;

            /* Use server-provided total for pagination, fall back to
             * episode_count if the server doesn't include it. */
            cJSON *total_json = cJSON_GetObjectItem(root, "total");
            if (total_json && total_json->type == cJSON_Number)
                g_feed_result.total_episodes = (int)total_json->valuedouble;
            else
                g_feed_result.total_episodes = n;

            g_feed_ok = true;
            cJSON_Delete(root);
        } else {
            g_feed_ok = false;
        }
        http_free_response_body(body);
        g_feed_done = true;
    }
}

/* ── Download worker ────────────────────────────────────────────────────── */

#define DL_BASE_PATH    "/sdcard/.podcast/downloads"

static TaskHandle_t  g_dl_task_handle = NULL;
static SemaphoreHandle_t g_dl_sem = NULL;   /* wake signal: "there may be PENDING work" */
static bool          g_dl_running = false;

/* Live download speed shared to the status bar. */
static volatile int  g_dl_speed_bps = 0;
static volatile bool g_dl_active    = false;

/* ── ETA telemetry (measured throughput) ────────────────────────────────────
 * The download-task page estimates time-left as (remaining bytes / avg speed)
 * instead of a hard-coded duration multiplier. All values are updated on CPU1
 * (worker) and read on CPU0 (LVGL); 32-bit aligned int access is atomic on the
 * Xtensa core, matching the existing lock-free stance for g_dl_speed_bps. */
static volatile int  g_dl_avg_bps            = 0;  /* EMA of measured speed (B/s) */
static volatile int  g_dl_cur_remaining      = 0;  /* in-flight bytes left (0=idle/unknown) */
static volatile int  g_dl_bytes_per_audio_sec = 0; /* learned file B per audio-second */

/* Set by the worker (CPU1) when a task changes to a non-success terminal state
 * (FAILED); polled + cleared by controller_process_download (CPU0) to fire a UI
 * refresh. Completions use the done-ring instead, so this stays failure-only and
 * never double-refreshes a success. */
static volatile bool g_dl_status_dirty       = false;

/* Retry a transient transfer failure this many times before giving up. Connect
 * aborts (ESP_ERR_HTTP_CONNECT) are usually momentary Wi-Fi/proxy hiccups. */
#define DL_MAX_ATTEMPTS 3

/* Cancellation of the in-flight transfer. The worker publishes the task_id it
 * is actively downloading; a cancel request sets g_dl_abort_current, which
 * dl_progress_cb reports back to http_download_to_file to stop the transfer. */
static volatile int  g_dl_current_task_id = -1;
static volatile bool g_dl_abort_current   = false;
/* Playback priority: internal DRAM can't hold audio decode + streaming download
 * + SD DMA at once (sdmmc "not enough mem"). When audio is playing, the in-flight
 * download is aborted and kept PENDING; it resumes after playback stops.
 * dl_progress_cb sets this when it detects playback mid-transfer. */
static volatile bool g_dl_yield_to_audio  = false;

/* Completed-download hand-off ring (worker CPU1 → controller_process_download
 * CPU0). Single-producer / single-consumer; carries only the task_id so the
 * LVGL thread can fire the completion event + add to the local cache. */
#define DL_DONE_RING    32
static volatile int  g_dl_done_ids[DL_DONE_RING];
static volatile int  g_dl_done_head = 0;   /* advanced by worker (producer) */
static volatile int  g_dl_done_tail = 0;   /* advanced by process_download (consumer) */

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

static bool dl_progress_cb(int bytes_done, int total_bytes, int speed_bps)
{
    g_dl_speed_bps = speed_bps;

    /* EMA-smooth the bursty instantaneous speed (3:1) → a stable ETA divisor.
     * Carried across tasks: throughput is a property of the link, not the file. */
    if (speed_bps > 0)
        g_dl_avg_bps = g_dl_avg_bps > 0 ? (g_dl_avg_bps * 3 + speed_bps) / 4
                                        : speed_bps;

    if (total_bytes > 0) {
        int rem = total_bytes - bytes_done;
        g_dl_cur_remaining = rem > 0 ? rem : 0;

        PodcastModel *m = g_podcast_app.model;
        int idx = m ? dl_find_index(m, g_dl_current_task_id) : -1;
        if (idx >= 0) {
            DownloadTask *t = &m->download_tasks[idx];
            /* Live % — RAM only; persisted at completion, not every 500ms tick
             * (an SD write per tick would thrash the card). */
            t->progress = (int)((int64_t)bytes_done * 100 / total_bytes);
            /* Learn bytes-per-audio-second to size queued (not-yet-open) tasks. */
            if (t->duration_sec > 0) {
                int r = total_bytes / t->duration_sec;
                g_dl_bytes_per_audio_sec = g_dl_bytes_per_audio_sec > 0
                    ? (g_dl_bytes_per_audio_sec * 3 + r) / 4 : r;
            }
        }
    } else {
        g_dl_cur_remaining = 0;   /* chunked / size unknown → ETA falls back to estimate */
    }

    if (g_dl_abort_current) return false;   /* user cancel */
    if (audio_player_is_active()) {         /* pipeline holds the memory → yield */
        g_dl_yield_to_audio = true;
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
    for (int i = 0; i < m->download_task_count; i++) {
        DownloadTask *t = &m->download_tasks[i];
        if (t->status != DOWNLOAD_STATUS_PENDING) continue;
        if (!t->audio_url[0] || !t->file_path[0]) continue;
        snprintf(url,  url_sz,  "%s", t->audio_url);
        snprintf(path, path_sz, "%s", t->file_path);
        int id = t->id;
        t->status   = DOWNLOAD_STATUS_DOWNLOADING;
        t->progress = 0;
        task_store_update(app, id);
        return id;
    }
    return -1;
}

/* Producer side of the completion ring (worker/CPU1). */
static void dl_done_push(int task_id)
{
    int nh = (g_dl_done_head + 1) % DL_DONE_RING;
    if (nh == g_dl_done_tail) return;   /* ring full — consumer lagging; drop notify */
    g_dl_done_ids[g_dl_done_head] = task_id;
    g_dl_done_head = nh;
}

static void dl_worker_task(void *arg)
{
    (void)arg;
    /* Static scratch — only the worker touches these, one transfer at a time.
     * Kept off the 10KB stack (url 1KB + path 0.5KB). */
    static char s_url[1024];
    static char s_path[512];

    while (g_dl_running) {
        /* Wait for a wake, timeout 5s to re-scan / check the running flag. */
        xSemaphoreTake(g_dl_sem, pdMS_TO_TICKS(5000));

        /* Drain all PENDING tasks this wake (serial — one transfer at a time).
         * dl_take_next_pending re-scans the model each iteration, so a task
         * added mid-drain is picked up here too. */
        while (g_dl_running) {
            PodcastApp *app = &g_podcast_app;
            if (!app || !app->model) break;

            /* Playback priority: don't START a download while the audio pipeline
             * is alive (playing/paused/tearing down). Only resumes once it's fully
             * torn down and the internal DRAM is freed. Tasks stay PENDING. */
            if (audio_player_is_active()) break;

            int task_id = dl_take_next_pending(app, s_url, sizeof(s_url),
                                                     s_path, sizeof(s_path));
            if (task_id < 0) break;   /* nothing pending */

            ESP_LOGI(TAG, "dl: downloading task %d → %s", task_id, s_path);

            g_dl_current_task_id = task_id;
            g_dl_abort_current   = false;
            g_dl_yield_to_audio  = false;
            g_dl_active          = true;
            /* Retry transient failures (connect abort, dropped socket) a few
             * times before marking FAILED — but stop immediately if the user
             * cancelled (g_dl_abort_current) or audio started (g_dl_yield_to_audio). */
            bool ok = false;
            for (int attempt = 1; attempt <= DL_MAX_ATTEMPTS; attempt++) {
                ok = http_download_to_file(s_url, s_path, dl_progress_cb);
                if (ok || g_dl_abort_current || g_dl_yield_to_audio) break;
                if (attempt < DL_MAX_ATTEMPTS) {
                    ESP_LOGW(TAG, "dl: task %d attempt %d/%d failed — retrying",
                             task_id, attempt, DL_MAX_ATTEMPTS);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
            g_dl_active          = false;
            g_dl_current_task_id = -1;
            g_dl_speed_bps       = 0;
            g_dl_cur_remaining   = 0;

            /* Re-find by id — the task may have been cancelled/deleted while the
             * transfer ran (its model entry is gone). */
            int idx = dl_find_index(app->model, task_id);
            if (idx < 0) {
                /* Cancelled mid-flight: on abort http already unlinked the
                 * partial; if it finished before the cancel landed, remove the
                 * now-orphaned file. */
                remove(s_path);
                ESP_LOGI(TAG, "dl: task %d cancelled — file removed", task_id);
                continue;
            }
            if (ok) {
                app->model->download_tasks[idx].status   = DOWNLOAD_STATUS_COMPLETED;
                app->model->download_tasks[idx].progress = 100;
                task_store_update(app, task_id);
                dl_done_push(task_id);   /* hand off to CPU0 for event + cache */
                ESP_LOGI(TAG, "dl: task %d OK", task_id);
            } else if (g_dl_yield_to_audio) {
                /* Paused for playback: keep PENDING (in RAM only — avoid an SD
                 * write while audio holds the memory) and stop draining. Resumes
                 * on the next re-scan after playback ends; on reboot dl_resume
                 * turns the persisted DOWNLOADING back into PENDING. Re-downloads
                 * from the start (the partial was unlinked on abort). */
                app->model->download_tasks[idx].status   = DOWNLOAD_STATUS_PENDING;
                app->model->download_tasks[idx].progress = 0;
                ESP_LOGI(TAG, "dl: task %d paused for playback (will resume)", task_id);
                break;
            } else {
                app->model->download_tasks[idx].status = DOWNLOAD_STATUS_FAILED;
                task_store_update(app, task_id);
                g_dl_status_dirty = true;   /* nudge CPU0 to refresh the stats card */
                ESP_LOGW(TAG, "dl: task %d FAILED after %d attempts",
                         task_id, DL_MAX_ATTEMPTS);
            }
        }
    }
    vTaskDelete(NULL);
}

/* ── Resume pending downloads after reboot / WiFi reconnect ────────────── */

void podcast_controller_resume_downloads(struct PodcastApp *app) { dl_resume_pending(app); }

static void dl_resume_pending(PodcastApp *app)
{
    if (!app || !app->model) return;
    PodcastModel *m = app->model;

    /* Ensure the worker is running */
    if (!g_dl_running) dl_worker_start();

    int pending = 0;
    for (int i = 0; i < m->download_task_count; i++) {
        DownloadTask *t = &m->download_tasks[i];
        if (t->status == DOWNLOAD_STATUS_DOWNLOADING) {
            /* Was mid-download when power cut — reset to PENDING */
            t->status = DOWNLOAD_STATUS_PENDING;
            t->progress = 0;
            task_store_update(app, t->id);
        }
        if (t->status == DOWNLOAD_STATUS_PENDING &&
            t->audio_url[0] && t->file_path[0])
            pending++;
    }

    /* One wake is enough — the worker drains every PENDING task it finds. */
    if (pending > 0 && g_dl_sem) {
        xSemaphoreGive(g_dl_sem);
        ESP_LOGI(TAG, "dl_resume: %d pending task(s), worker woken", pending);
    }
}

static void dl_worker_start(void)
{
    if (g_dl_running) return;
    g_dl_running = true;
    /* Counting semaphore used purely as a wake signal — the worker rescans
     * download_tasks on every wake, so surplus tokens only cause harmless
     * rescans and no wake is ever lost. */
    g_dl_sem = xSemaphoreCreateCounting(64, 0);
    /* Pin to CPU 1 — keeps SD card I/O and TCP stack callbacks off CPU 0
     * where LVGL rendering runs, avoiding audio dropouts and UI jank.
     * 10KB: func locals ~2.5KB + esp_http_client ~2KB + fwrite buf ~4KB
     * (url/path scratch are static, not on the stack). */
    xTaskCreatePinnedToCore(dl_worker_task, "podcast_dl", 10240, NULL,
                            1, &g_dl_task_handle, 1);
}

/* ── ETA telemetry getters (read by the download-task page on the LVGL thread) ── */
int podcast_dl_avg_speed_bps(void)           { return g_dl_avg_bps; }
int podcast_dl_current_task_id(void)         { return g_dl_current_task_id; }
int podcast_dl_current_remaining_bytes(void) { return g_dl_cur_remaining; }
int podcast_dl_bytes_per_audio_sec(void)     { return g_dl_bytes_per_audio_sec; }

void controller_process_download(void)
{
    PodcastApp *app = &g_podcast_app;
    if (!app || !app->model) return;

    /* Push live download state to the global status bar (LVGL thread) */
    lv_status_bar_t *sb = lv_status_bar_get();
    if (sb) {
        bool active = g_dl_active;
        lv_status_bar_set_download_speed(sb, active ? g_dl_speed_bps : -1);
        lv_status_bar_set_download_active(sb, active);
    }

    /* Drain completed-download notifications from the worker: the worker already
     * set status=COMPLETED; here (on the LVGL thread) we fire the UI event and
     * add the finished episode to the local cache. Looked up by task_id from the
     * model (the single source of truth). */
    PodcastModel *m = app->model;
    while (g_dl_done_tail != g_dl_done_head) {
        int task_id = g_dl_done_ids[g_dl_done_tail];
        g_dl_done_tail = (g_dl_done_tail + 1) % DL_DONE_RING;

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
    if (g_dl_status_dirty) {
        g_dl_status_dirty = false;
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

    for (int k = 0; k < n; k++) {
        int tid = ids[k];
        int idx = dl_find_index(m, tid);
        if (idx < 0) continue;

        DownloadTask *t = &m->download_tasks[idx];
        download_status_t s = t->status;
        if (s != DOWNLOAD_STATUS_PENDING && s != DOWNLOAD_STATUS_DOWNLOADING) continue;

        if (g_dl_active && g_dl_current_task_id == tid) {
            /* In-flight: signal the worker to abort. It stops the transfer,
             * unlinks its own partial file, and (finding the record gone) skips
             * the status write. */
            g_dl_abort_current = true;
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

    /* Remote http(s) → route through the proxy (handles TLS + 302 redirects).
     * /api/play transcodes to ADTS for the ADF pipeline;
     * /api/raw forwards raw bytes for download-to-SD. */
    podcast_proxy_url(out, out_sz, url, "/api/play");
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

    /* Build the SD destination path (shared with playback lookup). */
    char path[1536];
    podcast_local_audio_path(channel_title, title, path, sizeof(path));

    /* Ensure download worker is running */
    if (!g_dl_running) dl_worker_start();

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
    if (g_dl_sem) xSemaphoreGive(g_dl_sem);
    ESP_LOGI(TAG, "dl: task %d created '%s' → %s", task_id, title, path);
    return true;
}

/* ── Login ───────────────────────────────────────────────────────────── */

bool podcast_controller_login(struct PodcastApp *app, const char *n, const char *p,
    const char *cp, bool ag, const char **err) {
    if (!n||strlen(n)<2) {*err="Name too short";return false;}
    if (!p||strlen(p)<4) {*err="Password too short";return false;}
    if (strcmp(p,cp)) {*err="Mismatch";return false;}
    if (!ag) {*err="Agree";return false;}

    /* Determine endpoint: register if passwords differ (confirm-password mode),
     * otherwise login. */
    const char *endpoint = (cp != p) ? "/api/register" : "/api/login";

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
        if (root) {
            cJSON *e = cJSON_GetObjectItem(root, "error");
            if (e && e->valuestring) msg = e->valuestring;
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
