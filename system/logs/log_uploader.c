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
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "log_upload";

#define LOG_BASE_PATH       "/sdcard/.nomadcast/logs"
#define PODCAST_SERVER      "http://192.168.137.1:5000"
#define UPLOAD_URL          PODCAST_SERVER "/api/logs/chunk"
#define UPLOAD_CHUNK_BYTES  4096
#define MAX_RETRIES         3
/* Payload buffers live in PSRAM and the uploader has no large stack objects;
 * keep the task at the original 4 KB budget. */
#define UPLOAD_TASK_STACK   4096
#define MISSING_SESSION_CACHE 8

/* ── Upload job ─────────────────────────────────────────────────────────── */

typedef enum {
    UPLOAD_CMD_WIFI_CONNECTED = 1,
    UPLOAD_CMD_STOP           = 99,
} upload_cmd_t;

typedef struct {
    QueueHandle_t upload_queue;
    char missing_sessions[MISSING_SESSION_CACHE][64];
    size_t missing_count;
} log_uploader_state_t;

static log_uploader_state_t s_uploader;

static bool is_missing_session(const char *session)
{
    for (size_t i = 0; i < s_uploader.missing_count; ++i) {
        if (strcmp(s_uploader.missing_sessions[i], session) == 0) return true;
    }
    return false;
}

static void remember_missing_session(const char *session)
{
    if (is_missing_session(session)) return;
    if (s_uploader.missing_count < MISSING_SESSION_CACHE) {
        snprintf(s_uploader.missing_sessions[s_uploader.missing_count++], 64, "%s", session);
    } else {
        /* Ring-style replacement keeps the cache bounded. */
        memmove(s_uploader.missing_sessions, s_uploader.missing_sessions[1],
                (MISSING_SESSION_CACHE - 1) * sizeof(s_uploader.missing_sessions[0]));
        snprintf(s_uploader.missing_sessions[MISSING_SESSION_CACHE - 1], 64, "%s", session);
    }
}

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
        if (is_missing_session(base)) continue;

        /* FATFS can briefly expose a stale directory entry during mount or
         * concurrent cleanup. Only queue files that currently exist and have
         * content; this avoids entering the retry path for phantom/empty
         * sessions on every boot. */
        char log_path[128];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_BASE_PATH, base);
        struct stat log_st;
        if (stat(log_path, &log_st) != 0 || log_st.st_size <= 0) {
            char stale_idx[128];
            snprintf(stale_idx, sizeof(stale_idx), "%s/%s.idx", LOG_BASE_PATH, base);
            /* Logs are best-effort telemetry. Remove empty/phantom entries at
             * discovery time so they cannot reappear after a reboot. */
            unlink(log_path);
            unlink(stale_idx);
            continue;
        }

        /* Check if already fully uploaded */
        char idx_path[128];
        snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, base);

        FILE *idx = fopen(idx_path, "r");
        if (idx) {
            int marker = 0, last_line = 0;
            int retries = 0;
            int done = 0;
            int fields = fscanf(idx, "%d %d %d %d", &marker, &last_line, &retries, &done);
            fclose(idx);
            if (fields != 4 || marker != 2) { unlink(idx_path); continue; }

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
 * Returns true when the server advanced (or confirmed) the cursor.
 */

/* Upload raw, complete NDJSON bytes. The server owns parsing and storage;
 * this task only seeks to a byte offset and sends one small heap buffer. */
static bool upload_chunk(const char *session_base, int start_offset, int *next_offset)
{
    if (next_offset) *next_offset = start_offset;
    char log_path[128];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_BASE_PATH, session_base);

    FILE *f = fopen(log_path, "rb");
    if (!f) {
        int open_errno = errno;
        ESP_LOGE(TAG, "upload: cannot open session %s", log_path);
        /* Preserve errno across logging calls; ESP_LOGE may use stdio and
         * overwrite the thread-local errno value. */
        if (next_offset && open_errno == ENOENT) *next_offset = -2;
        return false;
    }

    if (fseek(f, start_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "upload: seek failed %s offset=%d", session_base, start_offset);
        fclose(f);
        return false;
    }
    char *payload = heap_caps_malloc(UPLOAD_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (!payload) {
        ESP_LOGE(TAG, "upload: payload allocation failed (%d bytes)", UPLOAD_CHUNK_BYTES);
        fclose(f);
        return false;
    }
    int bytes = (int)fread(payload, 1, UPLOAD_CHUNK_BYTES, f);
    fclose(f);
    if (bytes <= 0) {
        if (next_offset) *next_offset = -2;
        ESP_LOGW(TAG, "upload: session %s has no data at offset %d (fread=%d, errno=%d)",
                 session_base, start_offset, bytes, errno);
        free(payload);
        return false;
    }
    /* Never upload a partial final line; it may still be growing. */
    int complete = bytes;
    while (complete > 0 && payload[complete - 1] != '\n') complete--;
    if (complete == 0) {
        ESP_LOGW(TAG, "upload: session %s chunk has no complete line (bytes=%d, offset=%d)",
                 session_base, bytes, start_offset);
        free(payload);
        return false;
    }

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
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    char offset_header[24]; snprintf(offset_header, sizeof(offset_header), "%d", start_offset);
    esp_http_client_set_header(client, "Content-Type", "application/x-ndjson");
    esp_http_client_set_header(client, "X-Log-Session", session_base);
    esp_http_client_set_header(client, "X-Log-Offset", offset_header);

    /* The explicit content length is required here. Passing 0 causes
     * esp_http_client to send an empty request body even when a post field
     * was configured, which the server correctly rejects as an empty chunk. */
    esp_err_t err = esp_http_client_open(client, complete);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(payload);
        return false;
    }

    int written = esp_http_client_write(client, payload, complete);
    if (written != complete) {
        ESP_LOGE(TAG, "upload: write failed %s (%d/%d bytes)",
                 session_base, written, complete);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(payload);
        return false;
    }

    esp_err_t header_err = esp_http_client_fetch_headers(client);
    if (header_err < 0) {
        ESP_LOGE(TAG, "upload: fetch headers failed: %s", esp_err_to_name(header_err));
    }
    int status = esp_http_client_get_status_code(client);

    /* Read response body (small, for logging) */
    char resp[256] = {0};
    int response_bytes = esp_http_client_read(client, resp, sizeof(resp) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(payload);

    const char *p = strstr(resp, "\"next_offset\"");
    const char *colon = p ? strchr(p, ':') : NULL;
    int server_next = colon ? atoi(colon + 1) : start_offset + complete;
    if (next_offset) *next_offset = server_next;
    if (status == 200 || status == 409) {
        ESP_LOGI(TAG, "upload: %s bytes %d-%d → OK", session_base,
                 start_offset, start_offset + complete - 1);
        return true;
    } else {
        /* 410 is a deliberate server-side discard/quarantine response, not a
         * transport failure. Handle it before emitting the generic warning so
         * malformed historical sessions do not consume retries or look like
         * active faults. */
        if (status == 410 &&
            (strstr(resp, "discarded") || strstr(resp, "invalid_json") ||
             strstr(resp, "invalid_utf8"))) {
            if (next_offset) *next_offset = -1;
            ESP_LOGI(TAG, "upload: %s discarded by server", session_base);
            return false;
        }
        ESP_LOGW(TAG, "upload: %s HTTP %d (response=%d): %s", session_base,
                 status, response_bytes, resp);
        /* Historical sessions may contain malformed records from the old
         * logger. Do not let one corrupt file block the upload queue forever;
         * the server has already preserved the diagnostic and explicitly
         * identified this as a permanent content error. */
        if (status == 400 &&
            (strstr(resp, "invalid_json") || strstr(resp, "invalid_utf8"))) {
            if (next_offset) *next_offset = -1;
        }
        return false;
    }
}

/**
 * Update the .idx file for a session.
 * Format: <last_uploaded_byte> <retry_count> <done_flag>
 *   done_flag: 0 = in progress, 1 = complete
 */
static void update_idx(const char *session_base, int last_line, int retries, int done)
{
    char idx_path[128];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, session_base);

    FILE *f = fopen(idx_path, "w");
    if (!f) { ESP_LOGE(TAG, "upload: cannot write cursor %s", idx_path); return; }

    fprintf(f, "2 %d %d %d\n", last_line, retries, done);
    if (fflush(f) != 0 || fclose(f) != 0)
        ESP_LOGE(TAG, "upload: failed to persist cursor %s", idx_path);
}

/**
 * Read .idx file. Returns last_uploaded_byte (0 if not found), retries, done.
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

    int marker = 0;
    int n = fscanf(f, "%d %d %d %d", &marker, last_line, retries, done);
    fclose(f);
    if (n != 4 || marker != 2) { *last_line = 0; *retries = 0; *done = 0; }
}

/* ── Uploader task ──────────────────────────────────────────────────────── */

static void uploader_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uploader task started (prio=%d)", (int)uxTaskPriorityGet(NULL));

    while (1) {
        upload_cmd_t cmd;
        if (xQueueReceive(s_uploader.upload_queue, &cmd, portMAX_DELAY) != pdTRUE) {
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

            int last_offset, retries, done;
            read_idx(session, &last_offset, &retries, &done);

            if (done || retries >= MAX_RETRIES) {
                free(session);
                continue;
            }

            ESP_LOGI(TAG, "uploading %s (from byte %d, retry %d/%d)",
                     session, last_offset, retries, MAX_RETRIES);

            int new_last = last_offset;
            bool uploaded = upload_chunk(session, last_offset, &new_last);

            if (!uploaded && new_last == -1) {
                char bad_log[128];
                char bad_idx[128];
                snprintf(bad_log, sizeof(bad_log), "%s/%s.log", LOG_BASE_PATH, session);
                snprintf(bad_idx, sizeof(bad_idx), "%s/%s.idx", LOG_BASE_PATH, session);
                /* Permanently discard corrupt historical files. Keeping only
                 * an .idx marker is unreliable on some FATFS configurations
                 * and caused the same bad session to be rediscovered. */
                int log_rc = unlink(bad_log);
                int idx_rc = unlink(bad_idx);
                ESP_LOGW(TAG, "session %s discarded: invalid historical log content "
                         "(log=%d idx=%d)", session, log_rc, idx_rc);
                free(session);
                continue;
            }

            if (!uploaded && new_last == -2) {
                char stale_log[128];
                char stale_idx[128];
                snprintf(stale_log, sizeof(stale_log), "%s/%s.log", LOG_BASE_PATH, session);
                snprintf(stale_idx, sizeof(stale_idx), "%s/%s.idx", LOG_BASE_PATH, session);
                unlink(stale_log);
                unlink(stale_idx);
                remember_missing_session(session);
                ESP_LOGW(TAG, "session %s missing; removed stale cursor and suppressed repeats", session);
                free(session);
                continue;
            }

            if (uploaded && new_last > last_offset) {
                /* A closed session is complete when the uploaded offset reaches
                 * its current file size. The server owns line parsing. */
                char log_path[128];
                snprintf(log_path, sizeof(log_path), "%s/%s.log",
                         LOG_BASE_PATH, session);
                FILE *f = fopen(log_path, "r");
                long total_bytes = 0;
                if (f) {
                    fseek(f, 0, SEEK_END); total_bytes = ftell(f);
                    fclose(f);
                }

                int is_done = (total_bytes > 0 && new_last >= total_bytes) ? 1 : 0;
                update_idx(session, new_last, retries, is_done);

                if (is_done) {
                    ESP_LOGI(TAG, "session %s fully uploaded (%ld bytes)",
                             session, total_bytes);
                    /* Uploaded sessions no longer need local storage. Removing
                     * both files also avoids rediscovery if FATFS does not
                     * durably preserve the completion marker. */
                    unlink(log_path);
                    char idx_path[128];
                    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", LOG_BASE_PATH, session);
                    unlink(idx_path);
                }

                /* Yield between chunks — let WiFi serve higher-priority tasks */
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else {
                /* Upload failed — increment retry count */
                retries++;
                update_idx(session, last_offset, retries, 0);
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
    s_uploader.upload_queue = xQueueCreate(4, sizeof(upload_cmd_t));
    if (!s_uploader.upload_queue) {
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
    if (!s_uploader.upload_queue) return;
    upload_cmd_t cmd = UPLOAD_CMD_WIFI_CONNECTED;
    xQueueSend(s_uploader.upload_queue, &cmd, 0);  /* non-blocking */
}
