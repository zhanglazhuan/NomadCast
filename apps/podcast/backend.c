/**
 * @file backend.c
 * @brief Simple podcast backend — Apple Charts API only, sync HTTP
 *
 * No threads, no batch lookup, no filtering. Just fetches chart data
 * and converts to bk_channel_t list. Caller (controller) imports into model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "backend.h"
#include "i_http_client.h"
#include "cJSON.h"
#include "cache.h"
#include "hal.h"

/* ── Platform-agnostic logging ────────────────────────────────────────────── */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
static const char *T_BK = "podcast_bk";

#define BK_LOGI(...) ESP_LOGI(T_BK, __VA_ARGS__)
#define BK_LOGW(...) ESP_LOGW(T_BK, __VA_ARGS__)
#define BK_LOGE(...) ESP_LOGE(T_BK, __VA_ARGS__)
#else
#define BK_LOGI(...) printf(__VA_ARGS__)
#define BK_LOGW(...) fprintf(stderr, __VA_ARGS__)
#define BK_LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

#define ITUNES_SEARCH_URL   "https://itunes.apple.com/search"
#define ITUNES_LOOKUP_URL   "https://itunes.apple.com/lookup"

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void safe_str(char *dst, const char *src, int max)
{
    if (!dst || !src || max <= 0) return;
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void js_str(cJSON *obj, const char *key, char *dst, int max)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && item->type == cJSON_String && item->valuestring)
        safe_str(dst, item->valuestring, max);
    else dst[0] = '\0';
}

static int js_int(cJSON *obj, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (item->type == cJSON_Number) return (int)item->valuedouble;
    if (item->type == cJSON_String && item->valuestring) return atoi(item->valuestring);
    return def;
}

static void json_str_buf(char *dst, int max, const char *s) {
    int pos = strlen(dst); int remain = max - pos;
    pos += snprintf(dst + pos, remain, "\"");
    for (; *s && pos < max - 2; s++) {
        if (*s == '"' || *s == '\\') dst[pos++] = '\\';
        dst[pos++] = *s;
    }
    pos += snprintf(dst + pos, max - pos, "\"");
}

static int snprintf_safe(char *buf, int sz, const char *fmt, ...)
{
    int r; va_list a; va_start(a, fmt);
    r = vsnprintf(buf, sz, fmt, a); va_end(a);
    if (sz > 0) buf[sz - 1] = '\0';
    return r;
}

static char *url_encode(const char *str)
{
    int len = (int)strlen(str);
    char *out = (char *)malloc(len * 3 + 1);
    if (!out) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[j++] = (char)c;
        else if (c == ' ') out[j++] = '+';
        else { out[j++] = '%'; out[j++] = "0123456789ABCDEF"[c >> 4];
               out[j++] = "0123456789ABCDEF"[c & 0xF]; }
    }
    out[j] = '\0';
    return out;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void backend_init(void) {
    http_client_init();
}
void backend_deinit(void) { http_client_deinit(); }

/* ── Chart API: get top podcasts via iTunes Search ──────────────────── */

int backend_fetch_chart(bk_channel_t **out_channels, const char *country, int limit)
{
    *out_channels = NULL;

    int body_len = 0;
    char *body = NULL;

    /* Up to 2 attempts: cache first, then HTTP if cache is corrupt or missing.
     * On cJSON parse failure we purge the corrupt file and loop. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!body) {
            if (attempt == 0 && cache_is_valid(CACHE_CHART)) {
                body = cache_load(CACHE_CHART, &body_len);
                if (body) {
                    BK_LOGI("[BACKEND] chart from cache: %d bytes\n", body_len);
                }
            }
        }

        if (!body) {
            /* HTTP fetch */
            if (limit <= 0) limit = 50;
            char url[1024];
            int status = 0;
            snprintf_safe(url, sizeof(url),
                     "%s/api/charts/full?country=%s&limit=%d",
                     PODCAST_SERVER, country ? country : "cn", limit > 0 ? limit : 50);
            body = http_get_sync(url, &status, &body_len);
            BK_LOGI("[BACKEND] chart HTTP status=%d body_len=%d\n", status, body_len);
            if (!body || status != 200) {
                if (body) http_free_response_body(body);
                return 0;
            }
            cache_save(CACHE_CHART, body, body_len);
        }

        cJSON *root = cJSON_Parse(body);
        if (!root) {
            const char *err = cJSON_GetErrorPtr();
            BK_LOGE("cJSON fail at offset %d/%d, ctx='%.4s' — purging cache\n",
                    err ? (int)(err - body) : -1, body_len, body ? body : "(nil)");
            cache_purge(CACHE_CHART);  /* delete file + invalidate in-memory */
            http_free_response_body(body);
            body = NULL;
            continue;  /* retry → will go to HTTP */
        }

        /* ── Parse succeeded ──────────────────────────────────────────── */
        cJSON *feed   = cJSON_GetObjectItem(root, "feed");
        cJSON *results = feed ? cJSON_GetObjectItem(feed, "results") : NULL;
        int count = results ? cJSON_GetArraySize(results) : 0;
        if (count <= 0) { cJSON_Delete(root); http_free_response_body(body); return 0; }

        BK_LOGI("alloc %d channels, struct size=%u, total=%u\n",
                 count, (unsigned)sizeof(bk_channel_t), (unsigned)(count * sizeof(bk_channel_t)));
        BK_LOGI("free heap: internal=%u PSRAM=%u\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        bk_channel_t *albums = (bk_channel_t *)heap_caps_calloc(count, sizeof(bk_channel_t), MALLOC_CAP_SPIRAM);
        if (!albums) {
            BK_LOGE("calloc failed!\n");
            cJSON_Delete(root);
            http_free_response_body(body);
            return 0;
        }
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(results, i);
            bk_channel_t *a = &albums[i];
            a->collection_id = js_int(item, "id", 0);
            js_str(item, "name", a->title, BK_MAX_TITLE);
            js_str(item, "artistName", a->artist, BK_MAX_ARTIST);
            js_str(item, "artworkUrl100", a->artwork_url, BK_MAX_URL);
            js_str(item, "feedUrl", a->feed_url, BK_MAX_URL);
            if (!a->feed_url[0]) js_str(item, "url", a->feed_url, BK_MAX_URL);
            js_str(item, "primaryGenreName", a->genre, 64);
            if (!a->genre[0]) {
                cJSON *genres = cJSON_GetObjectItem(item, "genres");
                if (genres && cJSON_IsArray(genres)) {
                    cJSON *g0 = cJSON_GetArrayItem(genres, 0);
                    js_str(g0, "name", a->genre, 64);
                }
            }
            a->episode_count = js_int(item, "trackCount", 0);
            js_str(item, "releaseDate", a->release_date, sizeof(a->release_date));
        }

        *out_channels = albums;
        cJSON_Delete(root);
        http_free_response_body(body);
        return count;
    }

    return 0;  /* both attempts exhausted */
}

/* ── Search API ───────────────────────────────────────────────────────────── */

int backend_search_podcasts_sync(bk_channel_t **out_channels, const char *keyword,
                                  const char *country, int limit)
{
    *out_channels = NULL;
    char *enc = url_encode(keyword);
    if (!enc) return 0;

    char url[2048];
    if (limit <= 0) limit = 10;
    snprintf_safe(url, sizeof(url),
             "%s?term=%s&media=podcast&entity=podcast&country=%s&limit=%d",
             ITUNES_SEARCH_URL, enc, country ? country : "cn", limit);
    free(enc);

    int status, body_len;
    char *body = http_get_sync(url, &status, &body_len);
    if (!body || status != 200) { if (body) http_free_response_body(body); return 0; }

    /* Use simple string scanning to extract feedUrl (avoids cJSON bugs) */
    int count = 0;
    bk_channel_t *albums = NULL;
    int cap = 10;
    albums = (bk_channel_t *)calloc(cap, sizeof(bk_channel_t));

    const char *p = body;
    const char *end = body + body_len;
    while (p < end && count < cap) {
        /* Find "feedUrl":" */
        const char *f = strstr(p, "\"feedUrl\":\"");
        if (!f) break;

        /* Extract URL value */
        const char *vs = f + 11; /* past "feedUrl":" */
        const char *ve = vs;
        while (ve < end && *ve != '"') ve++;

        int ulen = (int)(ve - vs);
        if (ulen > 0 && ulen < BK_MAX_URL) {
            memcpy(albums[count].feed_url, vs, ulen);
            albums[count].feed_url[ulen] = '\0';
            count++;
        }
        p = ve + 1;
    }

    /* Also extract collectionName for the first result (for logging) */
    {
        const char *cn = strstr(body, "\"collectionName\":\"");
        if (cn) {
            const char *vs = cn + 18;
            const char *ve = vs;
            while (ve < end && *ve != '"') ve++;
            int nlen = (int)(ve - vs);
            if (nlen > 0 && nlen < BK_MAX_TITLE && count > 0) {
                memcpy(albums[0].title, vs, nlen);
                albums[0].title[nlen] = '\0';
            }
        }
    }

    *out_channels = albums;
    http_free_response_body(body);
    BK_LOGW("[BACKEND] search '%s': %d results\n", keyword, count);
    return count;
}

/* ── Batch Lookup (adds trackCount to chart data) ──────────────────────── */

int backend_lookup_batch_sync(const int *ids, int count, const char *country,
                               bk_channel_t **out_channels)
{
    *out_channels = NULL;
    if (!ids || count <= 0 || count > 50) return 0;

    /* Build ID list string */
    char id_str[2048] = "";
    int pos = 0;
    for (int i = 0; i < count && pos < (int)sizeof(id_str) - 16; i++) {
        if (i > 0) pos += snprintf(id_str + pos, sizeof(id_str) - pos, ",");
        pos += snprintf(id_str + pos, sizeof(id_str) - pos, "%d", ids[i]);
    }

    char url[2560];
    snprintf_safe(url, sizeof(url), "%s?id=%s&country=%s",
             ITUNES_LOOKUP_URL, id_str, country ? country : "cn");

    /* Check per-ID KV cache first, collect what we have */
    int body_len = 0;
    char *body = NULL;
    bool all_cached = true;
    for (int i = 0; i < count && all_cached; i++) {
        int ilen;
        char *item = cache_load_lookup_item(ids[i], &ilen);
        if (!item) all_cached = false;
        if (item) free(item);
    }

    if (!all_cached) {
        /* Fetch from network */
        int status;
        body = http_get_sync(url, &status, &body_len);
        if (!body || status != 200) { if (body) http_free_response_body(body); return 0; }
        /* Save each item to KV cache */
        /* Parse response to save individual items */
    } else {
        BK_LOGI("[BACKEND] lookup all cached (%d IDs)\n", count);
        /* Assemble body from KV cache — for simplicity, just return cached items as parsed array */
    }

    /* Fallback: if body is NULL but all_cached, we build from KV items.
     * For now, always go through the normal parse path by constructing the lookup response. */
    if (!body) {
        /* Build pseudo lookup JSON from KV cache */
        int est = count * 1500; /* ~1.5KB per item */
        body = (char *)malloc(est + 1);
        if (body) {
            int pos = snprintf(body, est, "{\"results\":[");
            for (int i = 0; i < count; i++) {
                int ilen;
                char *item = cache_load_lookup_item(ids[i], &ilen);
                if (item) {
                    if (i > 0) pos += snprintf(body + pos, est - pos, ",");
                    memcpy(body + pos, item, ilen); pos += ilen;
                    free(item);
                }
            }
            pos += snprintf(body + pos, est - pos, "]}");
            body_len = pos;
        }
    }

    /* Compact JSON (strip whitespace outside strings) */
    char *compact = (char *)malloc(body_len + 1);
    int clen = 0;
    if (compact) {
        bool in_str = false;
        for (int i = 0; i < body_len; i++) {
            char c = body[i];
            if (in_str) {
                compact[clen++] = c;
                if (c == '\\' && i + 1 < body_len) compact[clen++] = body[++i];
                else if (c == '"') in_str = false;
            } else if (c == '"') {
                compact[clen++] = c; in_str = true;
            } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                compact[clen++] = c;
            }
        }
        compact[clen] = '\0';
    }
    http_free_response_body(body);

    cJSON *root = compact ? cJSON_Parse(compact) : NULL;
    if (compact) free(compact);
    if (!root) return 0;

    cJSON *results = cJSON_GetObjectItem(root, "results");
    int n = results ? cJSON_GetArraySize(results) : 0;

    bk_channel_t *albums = NULL;
    if (n > 0) {
        albums = (bk_channel_t *)calloc(n, sizeof(bk_channel_t));
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(results, i);
            bk_channel_t *a = &albums[i];
            a->collection_id = js_int(item, "collectionId", 0);
            js_str(item, "collectionName", a->title, BK_MAX_TITLE);
            js_str(item, "artistName", a->artist, BK_MAX_ARTIST);
            js_str(item, "feedUrl", a->feed_url, BK_MAX_URL);
            js_str(item, "artworkUrl600", a->artwork_url, BK_MAX_URL);
            if (a->artwork_url[0] == '\0')
                js_str(item, "artworkUrl100", a->artwork_url, BK_MAX_URL);
            a->episode_count = js_int(item, "trackCount", 0);
            js_str(item, "releaseDate", a->release_date, sizeof(a->release_date));
        }
    }

    *out_channels = albums;

    /* Save each item to per-ID KV cache */
    char *item_json = (char *)malloc(4096);
    if (item_json) {
    for (int i = 0; i < n; i++) {
        int pos = snprintf(item_json, 4096,
            "{\"collectionId\":%d,\"collectionName\":", albums[i].collection_id);
        json_str_buf(item_json + pos, 4096 - pos, albums[i].title); pos = strlen(item_json);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"artistName\":");
        json_str_buf(item_json + pos, 4096 - pos, albums[i].artist); pos = strlen(item_json);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"feedUrl\":");
        json_str_buf(item_json + pos, 4096 - pos, albums[i].feed_url); pos = strlen(item_json);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"artworkUrl600\":");
        json_str_buf(item_json + pos, 4096 - pos, albums[i].artwork_url); pos = strlen(item_json);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"trackCount\":%d", albums[i].episode_count);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"releaseDate\":\"%s\"", albums[i].release_date);
        pos += snprintf(item_json + pos, 4096 - pos, ",\"primaryGenreName\":\"%s\"}", albums[i].genre);
        cache_save_lookup_item(albums[i].collection_id, item_json, (int)strlen(item_json));
    }
    free(item_json);
    }

    cJSON_Delete(root);
    return n;
}

/* ── Stubs for old async API ──────────────────────────────────────────────── */

void backend_search_podcasts(const char *kw, const char *cc, int lim,
                              bk_search_cb_t cb, bk_error_cb_t err, void *ud) { (void)kw; (void)cc; (void)lim; (void)cb; (void)err; (void)ud; }
void backend_get_top_podcasts(const char *cc, int lim, int gid,
                               bk_chart_cb_t cb, bk_error_cb_t err, void *ud) { (void)cc; (void)lim; (void)gid; (void)cb; (void)err; (void)ud; }
void backend_lookup_podcasts_batch(const int *ids, int n, const char *cc,
                                    bk_channels_cb_t cb, bk_error_cb_t err, void *ud) { (void)ids; (void)n; (void)cc; (void)cb; (void)err; (void)ud; }
