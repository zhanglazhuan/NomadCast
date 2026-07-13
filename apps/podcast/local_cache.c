/**
 * @file local_cache.c
 * @brief Local download library index — one JSON file per channel (bucketed).
 *
 * Layout: /sdcard/.podcast/downloads/.meta/<collection_id>.json
 *   { "cid":<canon>, "col":<collection_id>, "ch":"<title>",
 *     "episodes":[ {"eid","ep","url","dur"}, ... ] }
 *
 * Why buckets: a download completion rewrites only ONE small channel file
 * (O(episodes-in-channel)), not the whole library. The stable channel identity
 * is `collection_id` (falls back to the transient chart id `cid`), so the same
 * podcast always maps to the same file — channel dedup is inherent, and episode
 * dedup is by the stable `audio_url`. Each file is written atomically (tmp+rename).
 */
#include "local_cache.h"
#include "model.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#elif defined(ESP_PLATFORM)
#include <unistd.h>
#include "esp_log.h"
#define mkdir_p(p) mkdir(p, 0755)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

#define DL_META_DIR   "/sdcard/.podcast/downloads/.meta"        /* per-channel buckets */
#define DL_META_OLD   "/sdcard/.podcast/downloads/.meta.json"   /* legacy monolith (migration) */
#define DL_LOG_PATH   "/sdcard/.podcast/downloads/dl.log"

/* ── JSON helpers ──────────────────────────────────────────────────────── */

static bool json_get_str(const char *json, const char *key, char *out, int max) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < max - 1) {
        if (*p == '\\') p++;
        if (*p) out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static int json_get_int(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    return atoi(p);
}

static void json_write_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void downloads_ensure_dir(void) {
    char tmp[256] = "/sdcard/.podcast";
    mkdir_p(tmp);
    strcat(tmp, "/downloads");
    mkdir_p(tmp);
    strcat(tmp, "/.meta");     /* → DL_META_DIR */
    mkdir_p(tmp);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f); fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* ── In-RAM model update (shared by add / load / migrate) ─────────────────── */

/* Add one episode to the model, keyed by canonical channel id. Dedups the
 * episode by stable audio_url and the channel by canon. Returns true if a NEW
 * episode was inserted (so the caller knows to persist that channel's bucket). */
static bool local_model_add(struct PodcastApp *app, int canon, int col,
                            const char *ch_title, int eid, const char *ep_title,
                            const char *url, int dur) {
    PodcastModel *m = app->model;

    if (url && url[0])
        for (int i = 0; i < m->local_episode_count; i++)
            if (strcmp(m->local_episodes[i].audio_url, url) == 0) return false;  /* dup */

    int en = m->local_episode_count + 1;
    Episode *ne = (Episode *)realloc(m->local_episodes, en * sizeof(Episode));
    if (!ne) return false;
    m->local_episodes = ne;
    Episode *e = &m->local_episodes[m->local_episode_count];
    memset(e, 0, sizeof(Episode));
    e->id = eid; e->channel_id = canon; e->duration_sec = dur;
    snprintf(e->title, sizeof(e->title), "%s", ep_title ? ep_title : "");
    snprintf(e->audio_url, sizeof(e->audio_url), "%s", url ? url : "");
    m->local_episode_count = en;

    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == canon) { m->local_channels[i].episode_count++; return true; }

    int cn = m->local_channel_count + 1;
    Channel *nc = (Channel *)realloc(m->local_channels, cn * sizeof(Channel));
    if (nc) {
        m->local_channels = nc;
        Channel *c = &m->local_channels[m->local_channel_count];
        memset(c, 0, sizeof(Channel));
        c->id = canon; c->downloaded = true; c->episode_count = 1;
        c->collection_id = col;
        snprintf(c->title, sizeof(c->title), "%s", ch_title ? ch_title : "");
        c->category = CHANNEL_CATEGORY_NEWS_SOCIETY;
        c->card_color = 0x4CAF50 + (m->local_channel_count * 0x12345) % 0x1000000;
        m->local_channel_count = cn;
    }
    return true;
}

/* ── Bucket file I/O ──────────────────────────────────────────────────────── */

static void channel_file_path(char *out, int sz, int canon) {
    snprintf(out, sz, "%s/%d.json", DL_META_DIR, canon);
}

/* Atomically rewrite one channel's bucket from the current in-RAM model. */
static void local_write_channel(struct PodcastApp *app, int canon) {
    PodcastModel *m = app->model;
    const Channel *c = NULL;
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == canon) { c = &m->local_channels[i]; break; }
    if (!c) return;

    char path[320], tmp[328];
    channel_file_path(path, sizeof(path), canon);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "{\"cid\":%d,\"col\":%d,\"ch\":", canon, c->collection_id);
    json_write_str(f, c->title);
    fprintf(f, ",\"episodes\":[\n");
    bool first = true;
    for (int i = 0; i < m->local_episode_count; i++) {
        Episode *e = &m->local_episodes[i];
        if (e->channel_id != canon) continue;
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f, "{\"eid\":%d,\"ep\":", e->id); json_write_str(f, e->title);
        fprintf(f, ",\"url\":"); json_write_str(f, e->audio_url);
        fprintf(f, ",\"dur\":%d}", e->duration_sec);
    }
    fprintf(f, "\n]}\n");
    fflush(f);
#if defined(ESP_PLATFORM)
    fsync(fileno(f));      /* force write-through before the swap */
#endif
    fclose(f);

    remove(path);          /* FATFS rename fails if the destination exists */
    rename(tmp, path);     /* atomic-ish swap: the tmp already holds the full data */
}

/* Parse one bucket file's contents into the model. */
static void parse_channel_buf(struct PodcastApp *app, const char *buf) {
    int canon = json_get_int(buf, "cid", 0);
    int col   = json_get_int(buf, "col", 0);
    char ch_title[128];
    json_get_str(buf, "ch", ch_title, sizeof(ch_title));
    if (canon <= 0) return;

    const char *ep = buf;
    for (;;) {
        const char *obj = strstr(ep, "{\"eid\":");
        if (!obj) break;
        ep = obj + 1;
        int eid = json_get_int(obj, "eid", 0);
        int dur = json_get_int(obj, "dur", 0);
        static char eptitle[256], url[1024];   /* boot-time, single-threaded */
        json_get_str(obj, "ep",  eptitle, sizeof(eptitle));
        json_get_str(obj, "url", url, sizeof(url));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur);
    }
}

static void load_buckets(struct PodcastApp *app) {
    DIR *d = opendir(DL_META_DIR);
    if (!d) return;
    struct dirent *de;
    char path[320];
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;                 /* skip . .. and .tmp-hidden */
        size_t len = strlen(de->d_name);
        if (len < 6 || strcmp(de->d_name + len - 5, ".json") != 0) continue;  /* only *.json */
        snprintf(path, sizeof(path), "%s/%s", DL_META_DIR, de->d_name);
        char *buf = read_file(path);
        if (!buf) continue;
        parse_channel_buf(app, buf);
        free(buf);
    }
    closedir(d);
}

/* One-time: split the legacy monolithic .meta.json into per-channel buckets. */
static void migrate_old(struct PodcastApp *app) {
    char *buf = read_file(DL_META_OLD);
    if (!buf) { remove(DL_META_OLD); return; }

    const char *ep = buf;
    for (;;) {
        const char *obj = strstr(ep, "{\"cid\":");
        if (!obj) break;
        ep = obj + 1;
        int cid = json_get_int(obj, "cid", 0);
        int col = json_get_int(obj, "col", 0);
        int canon = (col > 0) ? col : cid;
        int eid = json_get_int(obj, "eid", 0);
        int dur = json_get_int(obj, "dur", 0);
        static char ch_title[128], eptitle[256], url[1024];
        json_get_str(obj, "ch",  ch_title, sizeof(ch_title));
        json_get_str(obj, "ep",  eptitle,  sizeof(eptitle));
        json_get_str(obj, "url", url,      sizeof(url));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur);
    }
    free(buf);

    for (int i = 0; i < app->model->local_channel_count; i++)
        local_write_channel(app, app->model->local_channels[i].id);
    remove(DL_META_OLD);   /* buckets are now the source of truth */
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool cache_local_has_episode(struct PodcastApp *app, int episode_id) {
    if (!app || !app->model || episode_id <= 0) return false;
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++)
        if (m->local_episodes[i].id == episode_id) return true;
    return false;
}

void cache_local_init(struct PodcastApp *app) {
    if (!app || !app->model) return;
    downloads_ensure_dir();

    struct stat st;
    if (stat(DL_META_OLD, &st) == 0) migrate_old(app);   /* upgrade legacy monolith once */
    else                            load_buckets(app);

    if (app->model->local_episode_count > 0) {
        app->model->local_has_content = true;
        app->model->local_sd_mounted  = true;
    }
}

void cache_local_add(struct PodcastApp *app,
                     int cid, const char *ch_title,
                     int eid, const char *ep_title,
                     const char *url, int dur,
                     const char *path, int collection_id) {
    (void)path;
    if (!app || !app->model) return;
    downloads_ensure_dir();

    /* Stable channel identity = collection_id (falls back to the transient cid). */
    int canon = (collection_id > 0) ? collection_id : cid;
    bool added = local_model_add(app, canon, collection_id, ch_title, eid, ep_title, url, dur);

    app->model->local_has_content = true;
    app->model->local_sd_mounted  = true;

    if (added) local_write_channel(app, canon);   /* rewrite only this channel's small file */

    FILE *logf = fopen(DL_LOG_PATH, "a");
    if (logf) {
        time_t now = time(NULL);
        char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(logf, "[%s] cid=%d ch='%s' eid=%d ep='%s' dur=%ds\n",
                ts, cid, ch_title ? ch_title : "", eid, ep_title ? ep_title : "", dur);
        fclose(logf);
    }
}
