/**
 * @file task_store.c
 * @brief Download-task persistence — one JSON file per task on SD card.
 *
 * Each task is a single-line JSON file under TASK_STORE_DIR.
 * Filename = %08d.json (task id), natural sort = FIFO order.
 * The worker picks the smallest-id PENDING task as the next download.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "task_store.h"
#include "app.h"
#include "esp_log.h"

static const char *TAG = "task_store";

/* Before SNTP sync, time(NULL) returns RTC uptime (a tiny value, e.g. < 1e6 s),
 * NOT real epoch time.  A task created while the clock was unsynced stores that
 * bogus value in created_at; on the next boot (clock now synced) it would be
 * mistaken for "expired 3 days ago" and dropped.  Guard TTL filtering so any
 * created_at below this floor is treated as "clock was unsynced" and kept. */
static const time_t TIME_FLOOR = 1000000000;   /* ~2001, far below any real date */

/* Ensure the directory exists (mkdir -p). */
static void ensure_dir(void) {
    struct stat st;
    if (stat(TASK_STORE_DIR, &st) == 0) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", TASK_STORE_DIR);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ── next-id counter ─────────────────────────────────────────────────────── */

static int read_next_id(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.next_id", TASK_STORE_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    int id = 1;
    fscanf(f, "%d", &id);
    fclose(f);
    return id;
}

static void write_next_id(int id) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.next_id", TASK_STORE_DIR);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", id);
    fclose(f);
}

/* ── JSON helpers (tiny, no cJSON dependency) ────────────────────────────── */

static void json_esc(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
}

static void task_to_json(FILE *f, const DownloadTask *t) {
    fprintf(f,
        "{\"id\":%d,\"ep\":\"", t->id);
    json_esc(f, t->episode_title);
    fprintf(f, "\",\"ch\":\"");
    json_esc(f, t->channel_title);
    fprintf(f, "\",\"st\":%d,\"prog\":%d,\"dur\":%d,"
            "\"ts\":%lld,\"path\":\"",
            (int)t->status, t->progress, t->duration_sec,
            (long long)t->created_at);
    json_esc(f, t->file_path);
    fprintf(f, "\",\"url\":\"");
    json_esc(f, t->audio_url);
    fprintf(f, "\",\"epid\":%d,\"chid\":%d,\"colid\":%d}\n",
            t->episode_id, t->channel_id, t->collection_id);
}

static bool task_from_json(const char *line, DownloadTask *t) {
    memset(t, 0, sizeof(*t));
    /* Quick hand-rolled parser — only needs to handle our own output format. */
    #define GET_INT(key, field) do { \
        const char *k = strstr(line, "\"" key "\":"); \
        if (k) { \
            k += strlen("\"" key "\":"); \
            if (*k == '"') k++;  /* tolerate quoted numerics, e.g. legacy "st":"2" */ \
            field = atoi(k); \
        } \
    } while(0)
    #define GET_LL(key, field) do { \
        const char *k = strstr(line, "\"" key "\":"); \
        if (k) field = (time_t)atoll(k + strlen("\"" key "\":")); \
    } while(0)
    #define GET_STR(key, field, sz) do { \
        const char *k = strstr(line, "\"" key "\":\""); \
        if (k) { \
            k += strlen("\"" key "\":\""); \
            int i = 0; \
            while (*k && *k != '"' && i < (int)(sz) - 1) { \
                if (*k == '\\') k++; \
                if (*k) field[i++] = *k++; \
            } \
            field[i] = '\0'; \
        } \
    } while(0)

    GET_INT("id",   t->id);
    GET_STR("ep",   t->episode_title, sizeof(t->episode_title));
    GET_STR("ch",   t->channel_title, sizeof(t->channel_title));
    GET_INT("st",   t->status);    /* stored as int, cast to enum */
    GET_INT("prog", t->progress);
    GET_INT("dur",  t->duration_sec);
    GET_LL("ts",    t->created_at);
    GET_STR("path", t->file_path, sizeof(t->file_path));
    GET_STR("url",  t->audio_url, sizeof(t->audio_url));
    GET_INT("epid", t->episode_id);
    GET_INT("chid", t->channel_id);
    GET_INT("colid",t->collection_id);
    return t->id > 0;
    #undef GET_INT
    #undef GET_LL
    #undef GET_STR
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

static bool write_task_file(int id, const DownloadTask *t) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%08d.json", TASK_STORE_DIR, id);
    /* Write directly to the final path (no tmp+rename).  FatFs flushes a
     * file's own directory entry on f_sync, but the directory-sector update
     * from f_rename is only written back lazily — so the old tmp+rename left
     * the JSON invisible after a power-cut/reboot ("Loaded 0 tasks" on every
     * boot).  A direct write is a single small sector; fsync makes it durable. */
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGE(TAG, "write %s failed", path); return false; }
    task_to_json(f, t);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(path); return false; }
    ESP_LOGI(TAG, "Wrote task %d (st=%d)", id, (int)t->status);
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int task_store_load(struct PodcastApp *app) {
    if (!app || !app->model) return 0;
    PodcastModel *m = app->model;
    podcast_model_download_lock(app);

    ensure_dir();

    DIR *d = opendir(TASK_STORE_DIR);
    if (!d) { podcast_model_download_unlock(app); return 0; }

    /* Count eligible tasks first */
    time_t cutoff = time(NULL) - TASK_TTL_DAYS * 86400;
    int count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;  /* skip .next_id, .gitkeep, .. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", TASK_STORE_DIR, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[2048];
        if (fgets(line, sizeof(line), f)) {
            DownloadTask tmp;
            if (task_from_json(line, &tmp) &&
                (tmp.created_at >= cutoff || tmp.created_at < TIME_FLOOR))
                count++;
        }
        fclose(f);
    }
    rewinddir(d);

    /* Allocate model array */
    free(m->download_tasks);
    m->download_tasks = count > 0
        ? (DownloadTask *)calloc(count, sizeof(DownloadTask)) : NULL;
    if (count > 0 && !m->download_tasks) { closedir(d); m->download_task_count = 0; podcast_model_download_unlock(app); return -1; }
    m->download_task_count = 0;

    /* Load tasks, sorted by filename (= task id) */
    while ((de = readdir(d)) != NULL && m->download_task_count < count) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", TASK_STORE_DIR, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[2048];
        if (fgets(line, sizeof(line), f)) {
            DownloadTask tmp;
            if (task_from_json(line, &tmp) &&
                (tmp.created_at >= cutoff || tmp.created_at < TIME_FLOOR)) {
                m->download_tasks[m->download_task_count++] = tmp;
            }
        }
        fclose(f);
    }
    closedir(d);

    /* Purge old tasks in background */
    task_store_purge_old();

    ESP_LOGI(TAG, "Loaded %d tasks (cutoff %lld)", m->download_task_count, (long long)cutoff);
    podcast_model_download_unlock(app);
    return m->download_task_count;
}

int task_store_create(struct PodcastApp *app,
                      const char *ep_title, const char *ch_title,
                      int duration_sec, const char *path,
                      const char *audio_url,
                      int episode_id, int channel_id, int collection_id)
{
    if (!app || !app->model) return -1;
    PodcastModel *m = app->model;
    podcast_model_download_lock(app);
    ensure_dir();

    int id = read_next_id();

    /* Grow model array */
    int n = m->download_task_count + 1;
    DownloadTask *dt = (DownloadTask *)realloc(m->download_tasks,
                                                n * sizeof(DownloadTask));
    if (!dt) { podcast_model_download_unlock(app); return -1; }
    if (!dt) return -1;
    m->download_tasks = dt;
    DownloadTask *t = &dt[m->download_task_count];
    memset(t, 0, sizeof(*t));

    t->id             = id;
    t->status         = DOWNLOAD_STATUS_PENDING;
    t->progress       = 0;
    t->duration_sec   = duration_sec;
    t->created_at     = time(NULL);
    t->episode_id     = episode_id;
    t->channel_id     = channel_id;
    t->collection_id  = collection_id;
    snprintf(t->episode_title, sizeof(t->episode_title), "%s", ep_title ? ep_title : "");
    snprintf(t->channel_title, sizeof(t->channel_title), "%s", ch_title ? ch_title : "");
    snprintf(t->file_path, sizeof(t->file_path), "%s", path ? path : "");
    snprintf(t->audio_url, sizeof(t->audio_url), "%s", audio_url ? audio_url : "");

    if (!write_task_file(id, t)) { memset(t, 0, sizeof(*t)); podcast_model_download_unlock(app); return -1; }
    write_next_id(id + 1);
    m->download_task_count = n;

    ESP_LOGI(TAG, "Created task %d: '%s'", id, ep_title);
    podcast_model_download_unlock(app);
    return id;
}

void task_store_update(struct PodcastApp *app, int task_id) {
    if (!app || !app->model) return;
    PodcastModel *m = app->model;
    podcast_model_download_lock(app);

    for (int i = 0; i < m->download_task_count; i++) {
        if (m->download_tasks[i].id == task_id) {
            (void)write_task_file(task_id, &m->download_tasks[i]);
            podcast_model_download_unlock(app);
            return;
        }
    }
    ESP_LOGW(TAG, "update: task %d not found in memory", task_id);
    podcast_model_download_unlock(app);
}

void task_store_delete(struct PodcastApp *app, int task_id) {
    if (!app || !app->model) return;
    PodcastModel *m = app->model;
    podcast_model_download_lock(app);

    /* Unlink file */
    char path[512];
    snprintf(path, sizeof(path), "%s/%08d.json", TASK_STORE_DIR, task_id);
    unlink(path);

    /* Compact model array */
    int found = -1;
    for (int i = 0; i < m->download_task_count; i++) {
        if (m->download_tasks[i].id == task_id) { found = i; break; }
    }
    if (found < 0) { podcast_model_download_unlock(app); return; }

    int tail = m->download_task_count - found - 1;
    if (tail > 0)
        memmove(&m->download_tasks[found], &m->download_tasks[found + 1],
                tail * sizeof(DownloadTask));
    m->download_task_count--;
    ESP_LOGI(TAG, "Deleted task %d", task_id);
    podcast_model_download_unlock(app);
}

void task_store_purge_old(void) {
    time_t cutoff = time(NULL) - TASK_TTL_DAYS * 86400;
    DIR *d = opendir(TASK_STORE_DIR);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", TASK_STORE_DIR, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) { unlink(path); continue; }

        char line[2048];
        bool expired = false;
        if (fgets(line, sizeof(line), f)) {
            DownloadTask tmp;
            if (task_from_json(line, &tmp) &&
                tmp.created_at < cutoff && tmp.created_at >= TIME_FLOOR)
                expired = true;
        }
        fclose(f);
        if (expired) {
            ESP_LOGI(TAG, "Purge old: %s", de->d_name);
            unlink(path);
        }
    }
    closedir(d);
}
