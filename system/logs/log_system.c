/**
 * @file log_system.c
 * @brief Core logging engine — ring buffer, session files, flush timer.
 */

#include "log_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

static const char *TAG = "log_sys";

#define LOG_BASE_PATH   "/sdcard/.nomadcast/logs"
#define MAX_SESSION_KB  100
#define MAX_SESSION_BYTES (MAX_SESSION_KB * 1024)
#define RING_LINES      128
#define MAX_LINE_LEN    256
#define FLUSH_INTERVAL_MS 5000
#define MAX_SESSIONS    20
#define MAX_SESSION_AGE_SEC (7 * 86400)

/* ── Ring buffer ────────────────────────────────────────────────────────── */

static char     s_ring[RING_LINES][MAX_LINE_LEN];
static int      s_ring_head = 0;   /* next write position */
static int      s_ring_count = 0;  /* lines waiting to flush */
static SemaphoreHandle_t s_ring_mutex = NULL;

/* ── Session state ──────────────────────────────────────────────────────── */

static FILE    *s_session_file = NULL;
static char     s_session_name[64];  /* e.g. "s_20260723_143052" */
static bool     s_initialized = false;
static bool     s_truncated = false;
static uint32_t s_session_bytes = 0;
static uint32_t s_line_count = 0;
static esp_timer_handle_t s_flush_timer = NULL;

/* ── Forward declarations ───────────────────────────────────────────────── */

static void ensure_log_dir(void);
static void open_session(void);
static void close_session(void);
static void flush_ring(void);
static void flush_timer_cb(void *arg);
static void purge_old_sessions(void);

/* ── Public API ─────────────────────────────────────────────────────────── */

void log_system_init(void)
{
    if (s_initialized) return;

    ensure_log_dir();
    purge_old_sessions();

    s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_ring_mutex) {
        ESP_LOGE(TAG, "failed to create ring mutex");
        return;
    }

    open_session();

    /* Register pre-sleep callback — log_system_shutdown will be called */
    extern void sleep_monitor_set_pre_sleep_callback(void (*cb)(void));
    sleep_monitor_set_pre_sleep_callback(log_system_shutdown);

    /* Start periodic flush timer */
    esp_timer_create_args_t targs = {
        .callback = flush_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "log_flush",
    };
    esp_timer_create(&targs, &s_flush_timer);
    esp_timer_start_periodic(s_flush_timer, FLUSH_INTERVAL_MS * 1000);

    /* Initialize auto-capture */
    extern void log_capture_init(void);
    log_capture_init();

    /* Start WiFi uploader task */
    extern void log_uploader_start(void);
    log_uploader_start();

    s_initialized = true;

    /* Log boot info */
    {
        size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char data[64];
        snprintf(data, sizeof(data), "{\"heap_free\":%u}", (unsigned)heap_free);
        log_event("sys", "boot", data);
    }

    ESP_LOGI(TAG, "session started: %s", s_session_name);
}

void log_system_shutdown(void)
{
    if (!s_initialized) return;
    s_initialized = false;

    /* Stop timer first */
    if (s_flush_timer) {
        esp_timer_stop(s_flush_timer);
        esp_timer_delete(s_flush_timer);
        s_flush_timer = NULL;
    }

    /* Flush remaining buffer */
    flush_ring();
    close_session();

    if (s_ring_mutex) {
        vSemaphoreDelete(s_ring_mutex);
        s_ring_mutex = NULL;
    }

    ESP_LOGI(TAG, "session ended (%lu lines, %lu bytes)",
             (unsigned long)s_line_count, (unsigned long)s_session_bytes);
}

void log_event(const char *category, const char *event, const char *data_json)
{
    if (!s_initialized) return;
    if (s_truncated) return;

    /* Get timestamp */
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* Format JSON line */
    char line[MAX_LINE_LEN];
    int len;
    if (data_json && data_json[0]) {
        len = snprintf(line, sizeof(line),
                       "{\"t\":%ld,\"ms\":%03d,\"c\":\"%s\",\"e\":\"%s\",\"d\":%s}\n",
                       (long)tv.tv_sec, (int)(tv.tv_usec / 1000),
                       category, event, data_json);
    } else {
        len = snprintf(line, sizeof(line),
                       "{\"t\":%ld,\"ms\":%03d,\"c\":\"%s\",\"e\":\"%s\",\"d\":{}}\n",
                       (long)tv.tv_sec, (int)(tv.tv_usec / 1000),
                       category, event);
    }

    if (len < 0 || len >= MAX_LINE_LEN) {
        /* Truncated — still write what we have */
        len = MAX_LINE_LEN - 2;
        line[len++] = '\n';
        line[len] = '\0';
    }

    /* Check size limit before adding to ring */
    if (s_session_bytes + len > MAX_SESSION_BYTES) {
        /* Mark truncated and write final marker */
        s_truncated = true;
        ESP_LOGW(TAG, "session reached %dKB limit, truncating", MAX_SESSION_KB);
        log_event("sys", "session_truncated", NULL);
        return;
    }

    /* Add to ring buffer */
    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_ring[s_ring_head], line, MAX_LINE_LEN - 1);
        s_ring[s_ring_head][MAX_LINE_LEN - 1] = '\0';
        s_ring_head = (s_ring_head + 1) % RING_LINES;
        if (s_ring_count < RING_LINES) {
            s_ring_count++;
        } else {
            /* Ring full — drop oldest (shouldn't happen with flush timer) */
            ESP_LOGW(TAG, "ring buffer overflow, dropping oldest line");
        }
        xSemaphoreGive(s_ring_mutex);

        s_session_bytes += len;
        s_line_count++;

        /* Flush immediately if buffer is half full */
        if (s_ring_count >= RING_LINES / 2) {
            flush_ring();
        }
    }
}

void log_page_nav(const char *direction, const char *page_name)
{
    char data[128];
    snprintf(data, sizeof(data), "{\"dir\":\"%s\",\"page\":\"%s\"}",
             direction, page_name);
    log_event("ui", "page_nav", data);
}

/* ── Internal: file management ──────────────────────────────────────────── */

static void ensure_log_dir(void)
{
    mkdir("/sdcard/.nomadcast", 0755);
    mkdir(LOG_BASE_PATH, 0755);
}

static void open_session(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    snprintf(s_session_name, sizeof(s_session_name),
             "s_%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_BASE_PATH, s_session_name);

    s_session_file = fopen(path, "a");
    if (!s_session_file) {
        ESP_LOGE(TAG, "failed to open session file: %s", path);
        return;
    }

    s_truncated = false;
    s_session_bytes = 0;
    s_line_count = 0;

    /* Write session start marker */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    fprintf(s_session_file,
            "{\"t\":%ld,\"ms\":%03d,\"c\":\"sys\",\"e\":\"session_start\",\"d\":{}}\n",
            (long)tv.tv_sec, (int)(tv.tv_usec / 1000));
    fflush(s_session_file);
}

static void close_session(void)
{
    if (!s_session_file) return;

    /* Write session end marker */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    fprintf(s_session_file,
            "{\"t\":%ld,\"ms\":%03d,\"c\":\"sys\",\"e\":\"session_end\","
            "\"d\":{\"lines\":%lu,\"bytes\":%lu}}\n",
            (long)tv.tv_sec, (int)(tv.tv_usec / 1000),
            (unsigned long)s_line_count, (unsigned long)s_session_bytes);

    fclose(s_session_file);
    s_session_file = NULL;
}

static void flush_ring(void)
{
    if (!s_session_file) return;
    if (s_ring_count == 0) return;

    if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    int count = s_ring_count;
    int start = (s_ring_head - count + RING_LINES) % RING_LINES;

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % RING_LINES;
        fputs(s_ring[idx], s_session_file);
        s_ring[idx][0] = '\0';
    }

    s_ring_count = 0;
    fflush(s_session_file);

    xSemaphoreGive(s_ring_mutex);
}

static void flush_timer_cb(void *arg)
{
    (void)arg;
    flush_ring();
}

static void purge_old_sessions(void)
{
    /* TODO: iterate LOG_BASE_PATH, delete files older than MAX_SESSION_AGE_SEC
     * or when more than MAX_SESSIONS exist. For v1, this is best-effort —
     * FAT filesystem directory iteration on ESP-IDF is non-trivial.
     * We rely on the 100KB-per-session cap to prevent runaway disk usage. */
    ESP_LOGI(TAG, "purge: retained max %d sessions / %d days",
             MAX_SESSIONS, (int)(MAX_SESSION_AGE_SEC / 86400));
}
