/**
 * @file log_uploader.c
 * @brief Low-priority WiFi log upload — scans session files, uploads
 *        incrementally with .idx resume markers.
 *
 * Priority 1 (idle) — never blocks UI, audio, or downloads.
 * Only uploads when WiFi is connected and no higher-priority task is active.
 */

#include "log_uploader.h"
#include "log_system.h"
#include "app_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "log_upload";

#define LOG_BASE_PATH       "/sdcard/.nomadcast/logs"
#define PODCAST_SERVER      "http://192.168.137.1:5000"
#define UPLOAD_URL          PODCAST_SERVER "/api/logs/upload"
#define MAX_CHUNK_LINES     64
#define MAX_RETRIES         3
#define UPLOAD_TASK_STACK   4096

/* ── Upload job ─────────────────────────────────────────────────────────── */

typedef enum {
    UPLOAD_CMD_WIFI_CONNECTED = 1,
    UPLOAD_CMD_STOP           = 99,
} upload_cmd_t;

static QueueHandle_t s_upload_queue = NULL;

/* ── Helpers ────────────────────────────────────────────────────────────── */

/**
 * Find the oldest session file that hasn't been fully uploaded.
 * Returns the basename (without .log extension) or NULL.
 * Caller must free the returned string.
 */
static char *find_pending_session(void)
{
    DIR *d = opendir(LOG_BASE_PATH);
    if (!d) return NULL;

    char *oldest = NULL;
    time_t oldest_time = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);

        /* Match s_YYYYMMDD_HHMMSS.log */
        if (len < 16 || strncmp(name, "s_", 2) != 0) continue;
        const char *ext = name + len - 4;
        if (strcmp(ext, ".log") != 0) continue;

        /* Extract basename */
        char base[64];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 4), name);

        /* Check if already fully uploaded */
        char idx_path[128];
        snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, base);

        FILE *idx = fopen(idx_path, "r");
        if (idx) {
            int last_line = 0;
            int retries = 0;
            int done = 0;
            fscanf(idx, "%d %d %d", &last_line, &retries, &done);
            fclose(idx);

            if (done || retries >= MAX_RETRIES) {
                continue;  /* skip completed or exhausted sessions */
            }
        }

        /* Parse timestamp from filename */
        struct tm tm = {0};
        int yr, mon, day, hr, min, sec;
        if (sscanf(base, "s_%04d%02d%02d_%02d%02d%02d",
                   &yr, &mon, &day, &hr, &min, &sec) != 6) continue;
        tm.tm_year = yr - 1900;
        tm.tm_mon  = mon - 1;
        tm.tm_mday = day;
        tm.tm_hour = hr;
        tm.tm_min  = min;
        tm.tm_sec  = sec;
        time_t t = mktime(&tm);

        if (!oldest || t < oldest_time) {
            oldest_time = t;
            free(oldest);
            oldest = strdup(base);
        }
    }
    closedir(d);
    return oldest;
}

/**
 * Upload one chunk of lines from a session file.
 * Returns the number of lines uploaded, or -1 on error.
 */
static int upload_chunk(const char *session_base, int start_line)
{
    char log_path[128];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_BASE_PATH, session_base);

    FILE *f = fopen(log_path, "r");
    if (!f) return -1;

    /* Read lines into buffer */
    char lines[MAX_CHUNK_LINES][512];
    int line_count = 0;
    int current_line = 0;
    char buf[512];

    while (fgets(buf, sizeof(buf), f) && line_count < MAX_CHUNK_LINES) {
        if (current_line >= start_line) {
            /* Strip trailing newline for clean concatenation */
            size_t blen = strlen(buf);
            if (blen > 0 && buf[blen - 1] == '\n') buf[blen - 1] = '\0';
            strncpy(lines[line_count], buf, sizeof(lines[0]) - 1);
            lines[line_count][sizeof(lines[0]) - 1] = '\0';
            line_count++;
        }
        current_line++;
    }
    fclose(f);

    if (line_count == 0) return 0;  /* nothing new to upload */

    /* Build JSON payload: {"session":"...","lines":[...]} */
    char *payload = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!payload) {
        ESP_LOGE(TAG, "upload: failed to allocate payload buffer");
        return -1;
    }

    int off = snprintf(payload, 4096,  /* first 4KB for header */
                       "{\"session\":\"%s\",\"lines\":[", session_base);

    for (int i = 0; i < line_count; i++) {
        if (off >= 7500) break;  /* safety margin */
        int n = snprintf(payload + off, 8192 - off,
                         "%s\"%s\"", (i > 0 ? "," : ""), lines[i]);
        if (n < 0) break;
        off += n;
    }
    snprintf(payload + off, 8192 - off, "]}");

    /* POST to server */
    esp_http_client_config_t cfg = {
        .url = UPLOAD_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "upload: failed to init HTTP client");
        free(payload);
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(payload);
        return -1;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Read response body (small, for logging) */
    char resp[256] = {0};
    esp_http_client_read(client, resp, sizeof(resp) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (status == 200) {
        ESP_LOGI(TAG, "upload: %s lines %d-%d → OK (%d lines)",
                 session_base, start_line, start_line + line_count - 1, line_count);
        return line_count;
    } else {
        ESP_LOGW(TAG, "upload: %s HTTP %d: %s", session_base, status, resp);
        return -1;
    }
}

/**
 * Update the .idx file for a session.
 * Format: <last_uploaded_line> <retry_count> <done_flag>
 *   done_flag: 0 = in progress, 1 = complete
 */
static void update_idx(const char *session_base, int last_line, int retries, int done)
{
    char idx_path[128];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, session_base);

    FILE *f = fopen(idx_path, "w");
    if (!f) return;

    fprintf(f, "%d %d %d\n", last_line, retries, done);
    fclose(f);
}

/**
 * Read .idx file. Returns last_uploaded_line (0 if not found), retries, done.
 */
static void read_idx(const char *session_base, int *last_line, int *retries, int *done)
{
    *last_line = 0;
    *retries = 0;
    *done = 0;

    char idx_path[128];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, session_base);

    FILE *f = fopen(idx_path, "r");
    if (!f) return;

    int n = fscanf(f, "%d %d %d", last_line, retries, done);
    fclose(f);
    if (n < 1) *last_line = 0;
}

/* ── Uploader task ──────────────────────────────────────────────────────── */

static void uploader_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uploader task started (prio=%d)", (int)uxTaskPriorityGet(NULL));

    while (1) {
        upload_cmd_t cmd;
        if (xQueueReceive(s_upload_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd == UPLOAD_CMD_STOP) break;
        if (cmd != UPLOAD_CMD_WIFI_CONNECTED) continue;

        ESP_LOGI(TAG, "WiFi connected — checking for pending uploads");

        /* Process one session at a time, with yield between chunks */
        while (1) {
            char *session = find_pending_session();
            if (!session) {
                ESP_LOGI(TAG, "no pending sessions to upload");
                break;
            }

            int last_line, retries, done;
            read_idx(session, &last_line, &retries, &done);

            if (done || retries >= MAX_RETRIES) {
                free(session);
                continue;
            }

            ESP_LOGI(TAG, "uploading %s (from line %d, retry %d/%d)",
                     session, last_line, retries, MAX_RETRIES);

            int uploaded = upload_chunk(session, last_line);

            if (uploaded > 0) {
                int new_last = last_line + uploaded;
                /* Check if we've reached end of file */
                char log_path[128];
                snprintf(log_path, sizeof(log_path), "%s/%s.log",
                         LOG_BASE_PATH, session);
                FILE *f = fopen(log_path, "r");
                int total_lines = 0;
                if (f) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), f)) total_lines++;
                    fclose(f);
                }

                int is_done = (new_last >= total_lines) ? 1 : 0;
                update_idx(session, new_last, retries, is_done);

                if (is_done) {
                    ESP_LOGI(TAG, "session %s fully uploaded (%d lines)",
                             session, total_lines);
                }

                /* Yield between chunks — let WiFi serve higher-priority tasks */
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else {
                /* Upload failed — increment retry count */
                retries++;
                update_idx(session, last_line, retries, 0);
                ESP_LOGW(TAG, "session %s upload failed (retry %d/%d)",
                         session, retries, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }

            free(session);
        }
    }

    ESP_LOGI(TAG, "uploader task stopped");
    vTaskDelete(NULL);
}

/* ── Public ──────────────────────────────────────────────────────────────── */

void log_uploader_start(void)
{
    s_upload_queue = xQueueCreate(4, sizeof(upload_cmd_t));
    if (!s_upload_queue) {
        ESP_LOGE(TAG, "failed to create upload queue");
        return;
    }

    xTaskCreate(uploader_task, "log_upload",
                UPLOAD_TASK_STACK, NULL,
                1,  /* priority 1 = idle (lowest above idle task) */
                NULL);
}

void log_uploader_on_wifi_connected(void)
{
    if (!s_upload_queue) return;
    upload_cmd_t cmd = UPLOAD_CMD_WIFI_CONNECTED;
    xQueueSend(s_upload_queue, &cmd, 0);  /* non-blocking */
}
