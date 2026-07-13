/**
 * @file http_esp.c
 * @brief ESP-IDF HTTP sync client — esp_http_client wrapper.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "hal.h"

static const char *TAG = "http_esp";

static char g_http_last_error[128];

const char *http_last_error(void) { return g_http_last_error; }

void http_client_init(void) { /* no-op */ }
void http_client_deinit(void) { /* no-op */ }

char *http_get_sync(const char *url, int *out_status, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 10,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(g_http_last_error, sizeof(g_http_last_error), "Failed to init HTTP client");
        ESP_LOGE(TAG, "%s", g_http_last_error);
        if (out_status) *out_status = 0;
        if (out_len)    *out_len = 0;
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(g_http_last_error, sizeof(g_http_last_error), "Connect failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Open failed: %s", esp_err_to_name(err));
        if (out_status) *out_status = 0;
        if (out_len)    *out_len = 0;
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "%s → HTTP %d, len=%d", url, status, content_len);

    if (out_status) *out_status = status;

    if (status != 200) {
        snprintf(g_http_last_error, sizeof(g_http_last_error), "HTTP %d", status);
        ESP_LOGW(TAG, "HTTP status %d, body may be empty", status);
        if (content_len <= 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (out_len) *out_len = 0;
            return NULL;
        }
    }

    int buf_cap = (content_len > 0) ? content_len + 1 : 16384;
    char *body = (char *)heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        snprintf(g_http_last_error, sizeof(g_http_last_error), "Out of memory (%d bytes)", buf_cap);
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
            if (!nb) { ESP_LOGE(TAG, "realloc(%d) failed", buf_cap); free(body); body = NULL; break; }
            body = nb;
        }
        int n = esp_http_client_read(client, body + total, buf_cap - total - 1);
        if (n <= 0) break;
        total += n;
    }
    if (body) body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (out_len) *out_len = total;
    g_http_last_error[0] = '\0';  /* clear on success */
    ESP_LOGI(TAG, "Downloaded %d bytes", total);
    return body;
}

char *http_post_json_sync(const char *url, const char *json_body,
                          int *out_status, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (out_status) *out_status = 0;
        if (out_len)    *out_len = 0;
        return NULL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    int body_len = (int)strlen(json_body);
    esp_http_client_set_post_field(client, json_body, body_len);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST open failed: %s", esp_err_to_name(err));
        if (out_status) *out_status = 0;
        if (out_len)    *out_len = 0;
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;

    int buf_cap = (content_len > 0) ? content_len + 1 : 1024;
    char *body = (char *)heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (out_len) *out_len = 0;
        return NULL;
    }

    int total = 0;
    while (1) {
        int remain = buf_cap - total - 1;
        if (remain < 256) {
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

    if (out_len) *out_len = total;
    return body;
}

void http_free_response_body(void *body)
{
    free(body);
}

/* ── Streaming download to file (for audio downloads) ──────────────────── */

#include <sys/stat.h>

bool http_download_to_file(const char *url, const char *file_path,
                           http_dl_progress_cb progress_cb)
{
    /* Ensure parent directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; }
    {
        char tmp[512]; snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
        }
        mkdir(tmp, 0755);
    }

    /* Follow redirects manually — audio CDNs return 302 to signed URLs */
    char current_url[1024];
    snprintf(current_url, sizeof(current_url), "%s", url);

    for (int redirect = 0; redirect < 5; redirect++) {
        esp_http_client_config_t cfg = {
            .url = current_url,
            .timeout_ms = 60000,
            .buffer_size = 8192,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .max_redirection_count = 0,  /* we handle redirects manually */
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "dl: failed to init client");
            return false;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "dl: open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }

        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 307 || status == 308) {
            /* Follow the redirect */
            char *loc = NULL;
            esp_http_client_get_header(client, "Location", &loc);
            if (loc && loc[0]) snprintf(current_url, sizeof(current_url), "%s", loc);
            ESP_LOGI(TAG, "dl: redirect %d → %s", status, current_url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        if (status != 200) {
            ESP_LOGE(TAG, "dl: HTTP %d for %s", status, current_url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        ESP_LOGI(TAG, "dl: %s → HTTP 200, len=%d", current_url, content_len);

        FILE *f = fopen(file_path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "dl: cannot open %s", file_path);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        /* Heap buffer — 8KB on task stack would risk overflow */
        char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        int total = 0, last_log = 0;
        bool first_chunk = true;
        bool aborted = false;
        int64_t win_start_us = esp_timer_get_time();  /* speed-window start */
        int     win_bytes    = 0;                      /* bytes since window start */
        if (buf) {
            while (1) {
                int n = esp_http_client_read(client, buf, 8192);
                if (n <= 0) break;
                if (first_chunk) {
                    ESP_LOGI(TAG, "dl: streaming started");
                    first_chunk = false;
                }
                fwrite(buf, 1, n, f);
                total     += n;
                win_bytes += n;
                /* Briefly yield every ~192KB so SPI LCD can grab the
                 * shared GDMA controller for page transitions.  1 ms
                 * overhead is negligible vs several-MB downloads. */
                if ((total & 0x2FFFF) == 0) vTaskDelay(1);

                /* ~500ms sliding window → instantaneous speed (bytes/sec) */
                int64_t now_us = esp_timer_get_time();
                int64_t win_us = now_us - win_start_us;
                if (win_us >= 500000) {
                    int speed_bps = (int)((int64_t)win_bytes * 1000000 / win_us);
                    if (progress_cb && !progress_cb(total, content_len, speed_bps)) {
                        ESP_LOGW(TAG, "dl: aborted by caller");
                        aborted = true;
                        break;
                    }
                    win_start_us = now_us;
                    win_bytes    = 0;
                }

                if (total - last_log >= 256 * 1024) {
                    ESP_LOGD(TAG, "dl: %d KB", total / 1024);
                    last_log = total;
                }
            }
            free(buf);
        }
        fclose(f);

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (aborted) {
            unlink(file_path);
            ESP_LOGW(TAG, "dl: removed partial file %s", file_path);
            return false;
        }

        if (total > 0) {
            ESP_LOGI(TAG, "dl: saved %d bytes to %s", total, file_path);
            return true;
        } else {
            unlink(file_path);
            return false;
        }
    }

    ESP_LOGE(TAG, "dl: too many redirects for %s", url);
    return false;
}
