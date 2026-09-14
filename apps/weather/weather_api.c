#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "zlib.h"
#include "cJSON.h"

#include "weather_api.h"
#include "lang.h"
#include "server_config.h"

static const char *TAG = "weather_api";

/* ── HTTP GET (PSRAM body, TLS via cert bundle) ────────────────────────────── */

/* Classify a failed open() into a user-facing reason. TLS failures are almost
 * always a clock that hasn't NTP-synced yet (cert "not yet valid"); a request
 * that ran out the full timeout is a reachability/timeout problem. */
static weather_api_err_t classify_failure(const char *url, int64_t elapsed_ms,
                                          int timeout_ms)
{
    if (elapsed_ms >= timeout_ms - 100) return WAPI_ERR_TIMEOUT;
    if (strncmp(url, "https://", 8) == 0 && time(NULL) < 1000000000L)
        return WAPI_ERR_TLS;   /* epoch-ish clock → cert validation fails */
    return WAPI_ERR_CONNECT;
}

/* ── gzip inflate ──────────────────────────────────────────────────────────── */

/* zlib allocator backed by PSRAM, so inflate's ~32 KB window never touches the
 * scarce internal DRAM. */
static void *zlib_psram_alloc(void *opaque, uInt items, uInt size) {
    (void)opaque;
    return heap_caps_malloc((size_t)items * size, MALLOC_CAP_SPIRAM);
}
static void zlib_psram_free(void *opaque, void *ptr) {
    (void)opaque;
    free(ptr);
}

/* QWeather's CDN always sends `Content-Encoding: gzip` (even when the client
 * requests identity), and esp_http_client does not decompress it. Inflate a
 * gzip stream into a freshly-allocated PSRAM buffer; returns NULL on failure
 * and sets *out_len to the uncompressed size on success. */
static char *gzip_inflate(const uint8_t *in, size_t in_len, size_t *out_len)
{
    *out_len = 0;
    if (!in || in_len < 18 || in[0] != 0x1f || in[1] != 0x8b) return NULL;

    /* ISIZE in the gzip trailer = uncompressed size (exact for these payloads). */
    uint32_t isize = (uint32_t)in[in_len - 4] | ((uint32_t)in[in_len - 3] << 8) |
                     ((uint32_t)in[in_len - 2] << 16) | ((uint32_t)in[in_len - 1] << 24);
    if (isize == 0) return NULL;

    char *out = (char *)heap_caps_malloc(isize + 1, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;

    /* windowBits = 16 + MAX_WBITS (31) makes inflate parse the gzip header,
     * deflate stream and CRC32/ISIZE trailer itself — no manual header work. */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.zalloc = zlib_psram_alloc;
    zs.zfree  = zlib_psram_free;
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        free(out);
        return NULL;
    }
    zs.next_in   = (Bytef *)in;
    zs.avail_in  = (uInt)in_len;
    zs.next_out  = (Bytef *)out;
    zs.avail_out = (uInt)isize;

    int ret = inflate(&zs, Z_FINISH);
    size_t written = zs.total_out;
    inflateEnd(&zs);
    ESP_LOGW(TAG, "gzip: in=%d isize=%u -> ret=%d written=%d",
             (int)in_len, (unsigned)isize, ret, (int)written);

    if (ret != Z_STREAM_END || written != isize) {
        free(out);
        return NULL;
    }
    out[written] = '\0';
    *out_len = written;
    return out;
}

static char *weather_http_get(const char *url, int *out_status, int *out_len,
                              weather_api_err_t *out_err, int timeout_ms)
{
    if (out_status) *out_status = 0;
    if (out_len)    *out_len = 0;
    if (out_err)    *out_err = WAPI_OK;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 10,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (out_err) *out_err = WAPI_ERR_CONNECT;
        return NULL;
    }

    int64_t t0 = esp_timer_get_time() / 1000;   /* ms */

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        int64_t elapsed = esp_timer_get_time() / 1000 - t0;
        ESP_LOGE(TAG, "open failed: %s (elapsed %lld ms)", esp_err_to_name(err), elapsed);
        esp_http_client_cleanup(client);
        if (out_err) *out_err = classify_failure(url, elapsed, timeout_ms);
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;
    char *enc = NULL;
    esp_http_client_get_header(client, "Content-Encoding", &enc);
    ESP_LOGW(TAG, "GET %s -> %d clen=%d enc=%s", url, status, content_len,
             enc ? enc : "-");
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_err) *out_err = WAPI_ERR_SERVER;
        return NULL;
    }

    int buf_cap = (content_len > 0) ? content_len + 1 : 16384;
    char *body = (char *)heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "malloc(%d) failed", buf_cap);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_len) *out_len = 0;
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

    if (body && total > 0) {
        ESP_LOGW(TAG, "body %d bytes head=%02x %02x %02x %02x", total,
                 (unsigned char)body[0],
                 (unsigned char)(total > 1 ? body[1] : 0),
                 (unsigned char)(total > 2 ? body[2] : 0),
                 (unsigned char)(total > 3 ? body[3] : 0));
    }

    /* The server may send gzip regardless of Accept-Encoding; inflate if so. */
    if (body && total >= 2 && (uint8_t)body[0] == 0x1f && (uint8_t)body[1] == 0x8b) {
        size_t olen = 0;
        char *dec = gzip_inflate((const uint8_t *)body, (size_t)total, &olen);
        if (dec) {
            free(body);
            body = dec;
            total = (int)olen;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_len) *out_len = total;
    return body;
}

/* ── cJSON access helpers ──────────────────────────────────────────────────── */

static double jnum(const cJSON *obj, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : 0.0;
}

/* QWeather returns numeric fields as JSON strings ("26"), unlike Open-Meteo's
 * raw numbers, so parse both shapes. */
static double jsnum(const cJSON *obj, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (it && cJSON_IsString(it)) return strtod(it->valuestring, NULL);
    if (it && cJSON_IsNumber(it)) return it->valuedouble;
    return 0.0;
}

static const char *jsstr(const cJSON *obj, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* Copy into a fixed buffer, always NUL-terminated. */
static void copy_str(char *dst, size_t n, const char *src) {
    if (!dst || !n) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
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

/* ── IP geolocation (provider chain) ──────────────────────────────────────── */

/* Build a short "city, region" / "city, country" display string from whatever
 * fields a provider returned. Prefer region (province/state) over country when
 * it differs from the city name. */
static void build_city_display(char *dst, size_t n,
                               const char *name, const char *region, const char *country)
{
    if (name && name[0] && region && region[0] && strcmp(region, name) != 0)
        snprintf(dst, n, "%s, %s", name, region);
    else if (name && name[0] && country && country[0] && strcmp(country, name) != 0)
        snprintf(dst, n, "%s, %s", name, country);
    else if (name && name[0])
        snprintf(dst, n, "%s", name);
    else
        snprintf(dst, n, "%s", (country && country[0]) ? country : "");
}

typedef bool (*ipgeo_parse_fn)(const char *body, double *lat, double *lon,
                               char *city, size_t city_len);

/* ip-api.com — keyless, plain HTTP, localized via `lang`, returns lat/lon
 * directly. `status` is a string ("success"/"fail"). */
static bool parse_ipapi(const char *body, double *lat, double *lon,
                        char *city, size_t city_len)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        const char *status = jsstr(root, "status");
        if (status && strcmp(status, "success") == 0) {
            double la = jnum(root, "lat");
            double lo = jnum(root, "lon");
            if (la != 0.0 || lo != 0.0) {
                if (lat) *lat = la;
                if (lon) *lon = lo;
                if (city && city_len)
                    build_city_display(city, city_len,
                                       jsstr(root, "city"),
                                       jsstr(root, "regionName"),
                                       jsstr(root, "country"));
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    return ok;
}

/* ipwho.is — HTTPS fallback (English only). `success` is a JSON boolean. */
static bool parse_ipwhois(const char *body, double *lat, double *lon,
                          char *city, size_t city_len)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
        if (success && cJSON_IsTrue(success)) {
            double la = jnum(root, "latitude");
            double lo = jnum(root, "longitude");
            if (la != 0.0 || lo != 0.0) {
                if (lat) *lat = la;
                if (lon) *lon = lo;
                if (city && city_len)
                    build_city_display(city, city_len,
                                       jsstr(root, "city"),
                                       jsstr(root, "region"),
                                       jsstr(root, "country"));
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    return ok;
}

static weather_api_err_t try_ipgeo(const char *url, ipgeo_parse_fn parse,
                                   double *lat, double *lon, char *city, size_t city_len)
{
    int status = 0, len = 0;
    weather_api_err_t herr = WAPI_OK;
    char *body = weather_http_get(url, &status, &len, &herr, 10000);
    if (!body) return herr;
    bool ok = parse(body, lat, lon, city, city_len);
    free(body);
    return ok ? WAPI_OK : WAPI_ERR_PARSE;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

weather_api_err_t weather_api_locate(double *lat, double *lon, char *city, size_t city_len)
{
    /* IP geolocation, in order of preference for a China network path:
     *   1. ip-api.com — keyless, plain HTTP (no TLS handshake/cert-bundle cost,
     *      which is what made the old HTTPS-only ipwho.is slow on-device),
     *      localized, returns coordinates directly.
     *   2. ipwho.is   — HTTPS fallback.
     * Stop at the first success; on total failure the caller falls back to
     * manual city selection. */
    const char *lang = (lang_get() == LANG_ZH_CN) ? "zh-CN" : "en";
    char url[256];

    snprintf(url, sizeof(url),
             "http://ip-api.com/json/?lang=%s&fields=status,lat,lon,city,regionName,country",
             lang);
    weather_api_err_t e = try_ipgeo(url, parse_ipapi, lat, lon, city, city_len);
    if (e == WAPI_OK) return WAPI_OK;

    e = try_ipgeo("https://ipwho.is/", parse_ipwhois, lat, lon, city, city_len);
    return e;
}

weather_api_err_t weather_api_forecast(double lat, double lon, weather_forecast_t *out)
{
    /* QWeather takes "location=longitude,latitude" (2 decimal places). */
    const char *lang = (lang_get() == LANG_ZH_CN) ? "zh" : "en";
    char url[512];
    int status = 0, len = 0;
    weather_api_err_t herr = WAPI_OK;
    cJSON *root;

    /* Current conditions. */
    snprintf(url, sizeof(url), "%s/v7/weather/now?location=%.2f,%.2f&key=%s&lang=%s",
             QWEATHER_HOST, lon, lat, QWEATHER_API_KEY, lang);
    char *body = weather_http_get(url, &status, &len, &herr, 15000);
    if (!body) return herr;

    bool ok = false;
    root = cJSON_Parse(body);
    if (root) {
        const char *code = jsstr(root, "code");
        if (code && strcmp(code, "200") == 0) {
            cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
            if (now) {
                out->temp       = (float)jsnum(now, "temp");
                out->feels_like = (float)jsnum(now, "feelsLike");
                out->humidity   = (float)jsnum(now, "humidity");
                out->wind       = (float)jsnum(now, "windSpeed");
                copy_str(out->text, sizeof(out->text), jsstr(now, "text"));
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    if (!ok) ESP_LOGW(TAG, "now parse FAILED, body head: %.120s", body);
    free(body);
    if (!ok) return WAPI_ERR_PARSE;

    /* 7-day forecast. */
    snprintf(url, sizeof(url), "%s/v7/weather/7d?location=%.2f,%.2f&key=%s&lang=%s",
             QWEATHER_HOST, lon, lat, QWEATHER_API_KEY, lang);
    body = weather_http_get(url, &status, &len, &herr, 15000);
    if (!body) return herr;

    ok = false;
    root = cJSON_Parse(body);
    if (root) {
        const char *code = jsstr(root, "code");
        if (code && strcmp(code, "200") == 0) {
            cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
            if (daily && cJSON_IsArray(daily)) {
                int n = cJSON_GetArraySize(daily);
                if (n > WEATHER_DAYS) n = WEATHER_DAYS;
                for (int i = 0; i < n; i++) {
                    cJSON *d = cJSON_GetArrayItem(daily, i);
                    if (!d) continue;
                    copy_str(out->daily_date[i], sizeof(out->daily_date[i]), jsstr(d, "fxDate"));
                    out->tmax[i] = (float)jsnum(d, "tempMax");
                    out->tmin[i] = (float)jsnum(d, "tempMin");
                    copy_str(out->daily_text[i], sizeof(out->daily_text[i]), jsstr(d, "textDay"));
                }
                ok = (n > 0);
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    return ok ? WAPI_OK : WAPI_ERR_PARSE;
}

weather_api_err_t weather_api_geocode(const char *query, weather_geo_result_t *results, int max, int *out_count)
{
    if (out_count) *out_count = 0;
    if (!query || !query[0]) return WAPI_ERR_PARSE;

    char enc[256];
    url_encode(query, enc, sizeof(enc));
    const char *lang = (lang_get() == LANG_ZH_CN) ? "zh" : "en";

    char url[512];
    snprintf(url, sizeof(url), "%s/geo/v2/city/lookup?location=%s&key=%s&lang=%s",
             QWEATHER_HOST, enc, QWEATHER_API_KEY, lang);

    int status = 0, len = 0;
    weather_api_err_t herr = WAPI_OK;
    char *body = weather_http_get(url, &status, &len, &herr, 15000);
    if (!body) return herr;

    bool ok = false;
    int count = 0;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        const char *code = jsstr(root, "code");
        if (code && strcmp(code, "200") == 0) {
            cJSON *loc = cJSON_GetObjectItemCaseSensitive(root, "location");
            if (loc && cJSON_IsArray(loc)) {
                int n = cJSON_GetArraySize(loc);
                if (n > max) n = max;
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(loc, i);
                    if (!item) continue;
                    const char *nm   = jsstr(item, "name");
                    if (!nm || !nm[0]) continue;
                    const char *adm1 = jsstr(item, "adm1");    /* province/state */
                    const char *co   = jsstr(item, "country");

                    char disp[WEATHER_CITY_MAX];
                    if (adm1 && adm1[0] && strcmp(adm1, nm) != 0)
                        snprintf(disp, sizeof(disp), "%s, %s", nm, adm1);
                    else if (co && co[0] && strcmp(co, nm) != 0)
                        snprintf(disp, sizeof(disp), "%s, %s", nm, co);
                    else
                        snprintf(disp, sizeof(disp), "%s", nm);

                    copy_str(results[count].name, sizeof(results[count].name), disp);
                    results[count].lat = jsnum(item, "lat");
                    results[count].lon = jsnum(item, "lon");
                    count++;
                }
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    if (out_count) *out_count = count;
    return ok ? WAPI_OK : WAPI_ERR_PARSE;
}
