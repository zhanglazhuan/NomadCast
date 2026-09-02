/**
 * @file cache.c
 * @brief Unified podcast data cache — API responses, downloads, playback position
 *
 * Directory structure:
 *   .nomadcast/cache/              — chart.json, lookup.json, artwork/
 *   .nomadcast/downloads/          — .meta.json (local episodes)
 *   .nomadcast/playback.json       — per-episode playback position
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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

/* ── Platform-agnostic logging ────────────────────────────────────────────── */
#ifdef ESP_PLATFORM
static const char *T_CACHE = "cache";
#define CACHE_LOGI(...) ESP_LOGI(T_CACHE, __VA_ARGS__)
#define CACHE_LOGW(...) ESP_LOGW(T_CACHE, __VA_ARGS__)
#define CACHE_LOGE(...) ESP_LOGE(T_CACHE, __VA_ARGS__)
#else
#define CACHE_LOGI(...) printf(__VA_ARGS__)
#define CACHE_LOGW(...) fprintf(stderr, __VA_ARGS__)
#define CACHE_LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

#define CACHE_DIR      "/sdcard/.nomadcast/cache"
#define META_FILE      "/sdcard/.nomadcast/cache/meta.json"
#define CHART_FILE     "/sdcard/.nomadcast/cache/chart.json"
#define LOOKUP_FILE    "/sdcard/.nomadcast/cache/lookup.json"

/* ── per-type state ──────────────────────────────────────────────────────── */

static struct {
    bool   valid;
    time_t saved_at;
} g_entries[CACHE_COUNT];

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void ensure_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir_p(tmp); *p = '/'; }
    }
    mkdir_p(tmp);
}

static const char *file_for(cache_type_t type) {
    switch (type) {
        case CACHE_CHART:  return CHART_FILE;
        case CACHE_LOOKUP: return LOOKUP_FILE;
        default:           return NULL;
    }
}

static const char *key_for(cache_type_t type) {
    switch (type) {
        case CACHE_CHART:  return "chart_at";
        case CACHE_LOOKUP: return "lookup_at";
        default:           return NULL;
    }
}

/* ── Shared JSON helpers (used by download & playback sections below) ───── */

static __attribute__((unused)) bool json_get_str(const char *json, const char *key, char *out, int max) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
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

static __attribute__((unused)) void json_write_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/* ── meta.json read/write ────────────────────────────────────────────────── */

static void meta_read(void) {
    FILE *f = fopen(META_FILE, "r");
    if (!f) return;

    char buf[512] = "";
    int len = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (len <= 0) return;
    buf[len] = '\0';

    time_t now = time(NULL);
    for (int t = 0; t < CACHE_COUNT; t++) {
        const char *key = key_for(t);
        if (!key) continue;

        char search[64];
        snprintf(search, sizeof(search), "\"%s\":", key);
        const char *p = strstr(buf, search);
        if (!p) continue;

        p += strlen(search);
        long long val = atoll(p);
        if (val > 0 && (now - (time_t)val) < CACHE_TTL_SECONDS) {
            g_entries[t].valid = true;
            g_entries[t].saved_at = (time_t)val;
        }
    }
}

static void meta_write(void) {
    ensure_dir(CACHE_DIR);
    FILE *f = fopen(META_FILE, "w");
    if (!f) return;

    fprintf(f, "{\"chart_at\":%lld,\"lookup_at\":%lld,\"ttl\":%d}\n",
            (long long)g_entries[CACHE_CHART].saved_at,
            (long long)g_entries[CACHE_LOOKUP].saved_at,
            CACHE_TTL_SECONDS);
    fclose(f);
}

/* ── public API ──────────────────────────────────────────────────────────── */

void cache_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    ensure_dir(CACHE_DIR);
    meta_read();
    CACHE_LOGI("[CACHE] chart=%s lookup=%s\n",
               g_entries[CACHE_CHART].valid  ? "valid" : "stale",
               g_entries[CACHE_LOOKUP].valid ? "valid" : "stale");
}

bool cache_is_valid(cache_type_t type) {
    return (type < CACHE_COUNT) && g_entries[type].valid;
}

char *cache_load(cache_type_t type, int *out_len) {
    *out_len = 0;
    const char *path = file_for(type);
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) { fclose(f); return NULL; }

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, sz, f);
    fclose(f);
    if (read != (size_t)sz) { free(buf); return NULL; }

    buf[sz] = '\0';
    *out_len = (int)sz;
    CACHE_LOGI("[CACHE] Loaded %s: %d bytes\n", path, (int)sz);
    return buf;
}

bool cache_save(cache_type_t type, const char *data, int len) {
    if (!data || len <= 0) return false;
    const char *path = file_for(type);
    if (!path) return false;

    ensure_dir(CACHE_DIR);

    FILE *f = fopen(path, "wb");
    if (!f) { CACHE_LOGE("[CACHE] Cannot write %s\n", path); return false; }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != (size_t)len) { CACHE_LOGE("[CACHE] Write incomplete %s\n", path); return false; }

    if (type < CACHE_COUNT) {
        g_entries[type].valid = true;
        g_entries[type].saved_at = time(NULL);
    }
    meta_write();
    CACHE_LOGI("[CACHE] Saved %s: %d bytes\n", path, len);
    return true;
}

void cache_invalidate(cache_type_t type) {
    if (type >= CACHE_COUNT) return;
    g_entries[type].valid = false;
    g_entries[type].saved_at = 0;
    meta_write();
    CACHE_LOGW("[CACHE] Invalidated type %d\n", (int)type);
}

void cache_purge(cache_type_t type) {
    const char *path = file_for(type);
    if (!path) return;
    cache_invalidate(type);
    unlink(path);
    CACHE_LOGW("[CACHE] Purged %s\n", path);
}

/* ── Lookup KV cache ─────────────────────────────────────────────────────── */

#define LOOKUP_KV_FILE "/sdcard/.nomadcast/cache/lookup_kv.json"

char *cache_load_lookup_item(int cid, int *out_len) {
    *out_len = 0;
    FILE *f = fopen(LOOKUP_KV_FILE, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 20*1024*1024) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f); fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';

    char key[32];
    snprintf(key, sizeof(key), "\"%d\":{", cid);
    char *start = strstr(buf, key);
    if (!start) { free(buf); return NULL; }
    start += strlen(key) - 1;

    int depth = 0;
    char *end = start;
    bool in_str = false;
    while (*end) {
        if (in_str) { if (*end == '\\') end++; else if (*end == '"') in_str = false; }
        else if (*end == '"') in_str = true;
        else if (*end == '{') depth++;
        else if (*end == '}') { depth--; if (depth == 0) { end++; break; } }
        end++;
    }
    int item_len = (int)(end - start);
    char *item = (char *)malloc(item_len + 1);
    if (item) { memcpy(item, start, item_len); item[item_len] = '\0'; *out_len = item_len; }
    free(buf);
    return item;
}

bool cache_save_lookup_item(int cid, const char *data, int len) {
    if (!data || len <= 0) return false;
    ensure_dir(CACHE_DIR);

    char *old = NULL;
    long old_sz = 0;
    FILE *fr = fopen(LOOKUP_KV_FILE, "rb");
    if (fr) {
        fseek(fr, 0, SEEK_END); old_sz = ftell(fr); fseek(fr, 0, SEEK_SET);
        if (old_sz > 0 && old_sz < 20*1024*1024) {
            old = (char *)malloc(old_sz + 1);
            if (old) { fread(old, 1, old_sz, fr); old[old_sz] = '\0'; }
        }
        fclose(fr);
    }

    FILE *fw = fopen(LOOKUP_KV_FILE, "wb");
    if (!fw) { free(old); return false; }

    bool wrote = false;
    if (old && old_sz > 5) {
        char key[32]; snprintf(key, sizeof(key), "\"%d\":{", cid);
        char *pos = strstr(old, key);
        if (pos) {
            int depth = 0; bool in_str = false;
            char *p = pos + strlen(key) - 1;
            while (*p) {
                if (in_str) { if (*p == '\\') p++; else if (*p == '"') in_str = false; }
                else if (*p == '"') in_str = true;
                else if (*p == '{') depth++;
                else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                p++;
            }
            fwrite(old, 1, pos - old, fw);
            fwrite(data, 1, len, fw);
            fwrite(p, 1, old_sz - (p - old), fw);
            wrote = true;
        }
    }
    if (!wrote) {
        if (old && old_sz > 5) {
            char *tail = old + old_sz;
            while (tail > old && (*tail == '\n' || *tail == '\r' || *tail == ' ')) tail--;
            if (tail > old && *tail == '}') tail--;
            fwrite(old, 1, tail - old, fw);
            fprintf(fw, ",\n\"%d\":", cid);
        } else {
            fprintf(fw, "{\n\"%d\":", cid);
        }
        fwrite(data, 1, len, fw);
        fprintf(fw, "\n}\n");
    }
    fclose(fw);
    free(old);
    return true;
}

/* ── Artwork cache ────────────────────────────────────────────────────────── */

#define ARTWORK_DIR "/sdcard/.nomadcast/cache/artwork"

bool cache_artwork_exists(int collection_id) {
    char path[256];
    snprintf(path, sizeof(path), ARTWORK_DIR "/%d.png", collection_id);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

bool cache_artwork_download(int collection_id, const char *url) {
    if (!url || !url[0]) return false;
    if (cache_artwork_exists(collection_id)) return true;

    char path[256];
    snprintf(path, sizeof(path), ARTWORK_DIR "/%d.png", collection_id);
    ensure_dir(ARTWORK_DIR);

#ifdef ESP_PLATFORM
    extern bool http_download_to_file(const char *download_url, const char *file_path,
                                      bool (*progress_cb)(int, int, int));
    /* Artwork is an image → use /api/raw (raw byte passthrough). /api/play
     * transcodes its input as audio and returns an empty body for images. */
    char proxy_url[1536];
    snprintf(proxy_url, sizeof(proxy_url), "http://192.168.137.1:5000/api/raw?url=%s", url);
    bool ok = http_download_to_file(proxy_url, path, NULL);
    if (ok) {
        CACHE_LOGI("[CACHE] Artwork saved: %s", path);
    } else {
        CACHE_LOGE("[CACHE] Artwork download failed: %s → %s", url, path);
    }
    return ok;
#else
    return false;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 * Playback position store (.nomadcast/playback.json)
 * ══════════════════════════════════════════════════════════════════════════ */

#define PB_PATH      "/sdcard/.nomadcast/playback.json"
#define PB_TMP       "/sdcard/.nomadcast/playback.tmp"
#define MAX_PB       512

static PlaybackEntry *g_pb_entries = NULL;
static int            g_pb_count   = 0;
static int            g_pb_cap     = 0;

static int pb_find_idx(int episode_id) {
    for (int i = 0; i < g_pb_count; i++)
        if (g_pb_entries[i].episode_id == episode_id) return i;
    return -1;
}

static int pb_find_or_create(int episode_id, int channel_id) {
    int idx = pb_find_idx(episode_id);
    if (idx >= 0) return idx;
    if (g_pb_count >= g_pb_cap) {
        int new_cap = g_pb_cap ? g_pb_cap * 2 : 64;
        if (new_cap > MAX_PB) new_cap = MAX_PB;
        if (g_pb_count >= new_cap) return -1;
        PlaybackEntry *nb = (PlaybackEntry *)realloc(g_pb_entries,
                                                      new_cap * sizeof(PlaybackEntry));
        if (!nb) return -1;
        g_pb_entries = nb; g_pb_cap = new_cap;
    }
    idx = g_pb_count++;
    memset(&g_pb_entries[idx], 0, sizeof(PlaybackEntry));
    g_pb_entries[idx].episode_id = episode_id;
    g_pb_entries[idx].channel_id = channel_id;
    return idx;
}

void cache_playback_init(struct PodcastApp *app) {
    (void)app;
    ensure_dir("/sdcard/.nomadcast/");

    FILE *f = fopen(PB_PATH, "rb");
    if (!f) { CACHE_LOGW("[CACHE] No playback.json yet\n"); return; }

    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f); fclose(f); buf[sz] = '\0';

    int count = 0;
    for (const char *p = buf; (p = strstr(p, "\"eid\":")); p++) count++;
    if (count == 0) { free(buf); return; }
    if (count > MAX_PB) count = MAX_PB;

    g_pb_entries = (PlaybackEntry *)calloc(count, sizeof(PlaybackEntry));
    g_pb_cap = count;

    char *ep = buf;
    for (int i = 0; i < count; i++) {
        char *obj = strstr(ep, "{\"eid\":");
        if (!obj) break;
        ep = obj + 1;
        PlaybackEntry *e = &g_pb_entries[g_pb_count++];
        e->episode_id   = json_get_int(obj, "eid", 0);
        e->channel_id   = json_get_int(obj, "cid", 0);
        e->position_sec = json_get_int(obj, "pos", 0);
        e->completed    = json_get_int(obj, "done", 0) != 0;
        e->last_updated = (int64_t)json_get_int(obj, "ts", 0);
    }
    free(buf);
    CACHE_LOGI("[CACHE] Loaded %d playback entries\n", g_pb_count);
}

void cache_playback_save(void) {
    ensure_dir("/sdcard/.nomadcast/");

    FILE *f = fopen(PB_TMP, "wb");
    if (!f) return;

    fprintf(f, "{\"entries\":[");
    for (int i = 0; i < g_pb_count; i++) {
        PlaybackEntry *e = &g_pb_entries[i];
        if (i > 0) fprintf(f, ",");
        fprintf(f, "{\"eid\":%d,\"cid\":%d,\"pos\":%d,\"done\":%d,\"ts\":%lld}",
                e->episode_id, e->channel_id, e->position_sec,
                e->completed ? 1 : 0, (long long)e->last_updated);
    }
    fprintf(f, "]}");
    fclose(f);
    remove(PB_PATH); rename(PB_TMP, PB_PATH);
    CACHE_LOGI("[CACHE] Saved %d playback entries\n", g_pb_count);
}

PlaybackEntry *cache_playback_get(int episode_id) {
    int idx = pb_find_idx(episode_id);
    return idx >= 0 ? &g_pb_entries[idx] : NULL;
}

bool cache_playback_is_completed(int episode_id) {
    PlaybackEntry *e = cache_playback_get(episode_id);
    return e ? e->completed : false;
}

int cache_playback_get_position(int episode_id) {
    PlaybackEntry *e = cache_playback_get(episode_id);
    return e ? e->position_sec : 0;
}

int cache_playback_total_sec(void) {
    int total = 0;
    for (int i = 0; i < g_pb_count; i++)
        total += g_pb_entries[i].position_sec;
    return total;
}

void cache_playback_set_position(int episode_id, int channel_id,
                                 int position_sec, int duration_sec) {
    int idx = pb_find_or_create(episode_id, channel_id);
    if (idx < 0) return;

    PlaybackEntry *e = &g_pb_entries[idx];
    e->position_sec = position_sec;
    e->channel_id   = channel_id;
    e->last_updated = (int64_t)time(NULL);

    if (duration_sec > 0 && position_sec >= (duration_sec * 9 / 10)
        && position_sec >= 30)
        e->completed = true;

    cache_playback_save();
}

void cache_playback_mark_completed(int episode_id, int channel_id,
                                   bool completed) {
    int idx = pb_find_or_create(episode_id, channel_id);
    if (idx < 0) return;

    g_pb_entries[idx].completed    = completed;
    g_pb_entries[idx].last_updated = (int64_t)time(NULL);
    cache_playback_save();
}

int cache_playback_channel_progress_pct(int channel_id, int total_episodes) {
    if (total_episodes <= 0) return 0;
    int done = 0;
    for (int i = 0; i < g_pb_count; i++)
        if (g_pb_entries[i].channel_id == channel_id && g_pb_entries[i].completed)
            done++;
    if (done >= total_episodes) return 100;
    return (done * 100) / total_episodes;
}
