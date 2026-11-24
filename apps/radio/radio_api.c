#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lvgl.h"

#include "radio_api.h"
#include "server_config.h"
#include "flash_store.h"

static const char *TAG = "radio_api";

/* ── HTTP GET (PSRAM body, always our server → always X-Api-Key) ─────────── */

static void url_encode(const char *src, char *dst, size_t dst_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 3 < dst_sz) {
        unsigned char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            dst[o++] = (char)ch;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[ch >> 4];
            dst[o++] = hex[ch & 0x0F];
        }
        p++;
    }
    dst[o] = '\0';
}

static char *radio_http_get(const char *url, int *out_status, radio_api_err_t *out_err,
                            int timeout_ms)
{
    if (out_status) *out_status = 0;
    if (out_err)    *out_err = RAPI_OK;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (out_err) *out_err = RAPI_ERR_CONNECT;
        return NULL;
    }

    /* The radio endpoints all live on our aggregation server. */
    esp_http_client_set_header(client, "X-Api-Key", NOMADCAST_API_KEY);
    char devid[24] = {0};
    if (flash_get_device_id(devid, sizeof(devid)) > 0)
        esp_http_client_set_header(client, "X-Device-Id", devid);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (out_err) *out_err = RAPI_ERR_CONNECT;
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;
    ESP_LOGW(TAG, "GET %s -> %d clen=%d", url, status, content_len);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_err) *out_err = RAPI_ERR_SERVER;
        return NULL;
    }

    int buf_cap = (content_len > 0) ? content_len + 1 : 16384;
    char *body = (char *)heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_err) *out_err = RAPI_ERR_CONNECT;
        return NULL;
    }

    int total = 0;
    while (1) {
        int remain = buf_cap - total - 1;
        if (remain < 1024) {
            buf_cap *= 2;
            char *nb = (char *)heap_caps_realloc(body, buf_cap, MALLOC_CAP_SPIRAM);
            if (!nb) { free(body); body = NULL; break; }
            body = nb;
        }
        int n = esp_http_client_read(client, body + total, buf_cap - total - 1);
        if (n <= 0) break;
        total += n;
    }
    if (body) body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return body;
}

/* ── cJSON access helpers ─────────────────────────────────────────────────── */

static const char *jsstr(const cJSON *obj, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static int jsint(const cJSON *obj, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsNumber(it)) ? it->valueint : 0;
}

static void copy_str(char *dst, size_t n, const char *src) {
    if (!dst || !n) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

/* The CJK display font (Harmony + montserrat fallback, set in main). Read-only
 * from the worker task — the font is const data, never freed at runtime. */
extern const lv_font_t *g_cjk_font;

/* Replace codepoints the display font can't render with a space, so the UI never
 * draws a "tofu" box for missing glyphs. Radio-Browser station names can contain
 * emoji / symbols outside the CJK font's unicode range. */
static void sanitize_for_display(char *dst, size_t n, const char *src)
{
    if (!dst || !n) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 1 < n) {
        uint32_t cp = 0;
        int len = 0;
        unsigned char c = *p;

        if (c < 0x80)                    { cp = c;            len = 1; }
        else if ((c & 0xE0) == 0xC0)     { cp = c & 0x1F;     len = 2; }
        else if ((c & 0xF0) == 0xE0)     { cp = c & 0x0F;     len = 3; }
        else if ((c & 0xF8) == 0xF0)     { cp = c & 0x07;     len = 4; }
        else { dst[o++] = ' '; p++; continue; }   /* invalid lead byte */

        int i;
        for (i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) break;      /* truncated sequence */
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (i < len) { dst[o++] = ' '; p++; continue; }

        bool ok = false;
        if (g_cjk_font) {
            lv_font_glyph_dsc_t dsc;
            ok = lv_font_get_glyph_dsc(g_cjk_font, &dsc, cp, 0);
        }
        if (ok) {
            if (o + (size_t)len >= n) break;
            memcpy(dst + o, p, len);
            o += len;
        } else {
            dst[o++] = ' ';
        }
        p += len;
    }
    dst[o] = '\0';
}

/* ── Public API ───────────────────────────────────────────────────────────── */

radio_api_err_t radio_api_regions(const char *country,
                                  radio_region_t *out, int max, int *out_count)
{
    if (out_count) *out_count = 0;
    if (!country || !country[0]) return RAPI_ERR_PARSE;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/radio/regions?country=%s",
             NOMADCAST_SERVER_BASE, country);

    int status = 0;
    radio_api_err_t herr = RAPI_OK;
    char *body = radio_http_get(url, &status, &herr, 15000);
    if (!body) return herr;

    bool ok = false;
    int count = 0;
    cJSON *root = cJSON_Parse(body);
    if (root && cJSON_IsArray(root)) {
        int n = cJSON_GetArraySize(root);
        if (n > max) n = max;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(root, i);
            if (!it) continue;
            copy_str(out[count].key, sizeof(out[count].key), jsstr(it, "key"));
            copy_str(out[count].name, sizeof(out[count].name), jsstr(it, "name"));
            out[count].order = jsint(it, "order");
            out[count].count = jsint(it, "count");
            count++;
        }
        ok = true;
    }
    cJSON_Delete(root);
    free(body);

    if (out_count) *out_count = count;
    return ok ? RAPI_OK : RAPI_ERR_PARSE;
}

radio_api_err_t radio_api_stations(const char *country, const char *region,
                                   radio_station_t *out, int max,
                                   int *out_count, int *out_total)
{
    if (out_count) *out_count = 0;
    if (out_total) *out_total = 0;
    if (!country || !country[0] || !region || !region[0]) return RAPI_ERR_PARSE;

    char enc[128];
    url_encode(region, enc, sizeof(enc));

    char url[512];
    snprintf(url, sizeof(url),
             "%s/api/radio/stations?country=%s&region=%s&order=popular&offset=0&limit=%d",
             NOMADCAST_SERVER_BASE, country, enc, max);

    int status = 0;
    radio_api_err_t herr = RAPI_OK;
    char *body = radio_http_get(url, &status, &herr, 20000);
    if (!body) return herr;

    bool ok = false;
    int count = 0, total = 0;
    cJSON *root = cJSON_Parse(body);
    if (root && cJSON_IsObject(root)) {
        total = jsint(root, "total");
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "stations");
        if (arr && cJSON_IsArray(arr)) {
            int n = cJSON_GetArraySize(arr);
            if (n > max) n = max;
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(arr, i);
                if (!it) continue;
                radio_station_t *s = &out[count];
                memset(s, 0, sizeof(*s));
                copy_str(s->uuid, sizeof(s->uuid), jsstr(it, "uuid"));
                sanitize_for_display(s->name, sizeof(s->name), jsstr(it, "name"));
                copy_str(s->region, sizeof(s->region), jsstr(it, "region"));
                copy_str(s->region_name, sizeof(s->region_name), jsstr(it, "region_name"));
                copy_str(s->codec, sizeof(s->codec), jsstr(it, "codec"));
                copy_str(s->favicon, sizeof(s->favicon), jsstr(it, "favicon"));
                s->votes   = jsint(it, "votes");
                s->bitrate = jsint(it, "bitrate");
                cJSON *hls = cJSON_GetObjectItemCaseSensitive(it, "hls");
                s->hls = (hls && cJSON_IsTrue(hls));
                if (!s->uuid[0]) continue;   /* skip malformed rows */
                count++;
            }
            ok = true;
        }
    }
    cJSON_Delete(root);
    free(body);

    if (out_count) *out_count = count;
    if (out_total) *out_total = total;
    return ok ? RAPI_OK : RAPI_ERR_PARSE;
}
