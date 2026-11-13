/*
 * NomadCast — System Clock
 */

#include "clock.h"
#include "app_event.h"
#include "flash_store.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "clock";

/* ── Timezone (POSIX TZ string — sign is inverted from UTC offset) ────────── */

#define DEFAULT_TZ "CST-8"   /* UTC+8 Beijing */

/* ── State ────────────────────────────────────────────────────────────────── */

typedef struct {
    bool synced;
    bool fmt24;
    bool sntp_started;
    char tz[32];
    int last_fired_minute;
    lv_timer_t *timer;
} clock_state_t;

static clock_state_t s_clock = {
    .fmt24 = true,
    .last_fired_minute = -1,
    .tz = DEFAULT_TZ,
};

/* ── WiFi event → trigger SNTP sync ───────────────────────────────────────── */

static void on_app_event(app_event_t event, const void *data)
{
    (void)data;
    if (event == APP_EVENT_WIFI_CONNECTED && !s_clock.sntp_started) {
        s_clock.sntp_started = true;
        clock_sync_sntp();
    }
}

/* ── SNTP sync callback ──────────────────────────────────────────────────── */

static void on_sntp_sync(struct timeval *tv)
{
    s_clock.synced = true;
    ESP_LOGI(TAG, "SNTP synced — epoch=%lld", (long long)tv->tv_sec);
}

/* ── LVGL 1-second timer ─────────────────────────────────────────────────── */

static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    int hour = s_clock.fmt24 ? tm.tm_hour
                       : (tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12);

    /* Fire tick event on minute change (not every second) */
    if (tm.tm_min != s_clock.last_fired_minute) {
        s_clock.last_fired_minute = tm.tm_min;
        app_event_clock_tick_t data = {
            .hour   = hour,
            .minute = tm.tm_min,
            .synced = s_clock.synced,
        };
        app_event_fire(APP_EVENT_CLOCK_TICK, &data);
    }
}

/* ── Apply timezone ──────────────────────────────────────────────────────── */

static void apply_timezone(void)
{
    if (s_clock.tz[0] != '\0') {
        setenv("TZ", s_clock.tz, 1);
    }
    tzset();
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void clock_init(void)
{
    /* Load saved timezone and format from flash so the correct settings
     * are active from boot, before the Settings app ever starts. */
    flash_get_str("settings", "tz", s_clock.tz, sizeof(s_clock.tz), DEFAULT_TZ);
    s_clock.fmt24 = flash_get_bool("settings", "fmt24", true);
    apply_timezone();

    /* At boot, try SNTP sync immediately (may fail if no WiFi yet).
     * On subsequent WiFi reconnects, APP_EVENT_WIFI_CONNECTED retriggers. */
    app_event_register(on_app_event);

    s_clock.timer = lv_timer_create(clock_tick_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_clock.timer, -1);

    ESP_LOGI(TAG, "Initialized (tz=%s, fmt=%s)",
             s_clock.tz, s_clock.fmt24 ? "24h" : "12h");
}

static void fire_immediate_tick(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int hour = s_clock.fmt24 ? tm.tm_hour
                       : (tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12);
    app_event_clock_tick_t data = { .hour = hour, .minute = tm.tm_min, .synced = s_clock.synced };
    app_event_fire(APP_EVENT_CLOCK_TICK, &data);
}

void clock_set_timezone(const char *tz)
{
    if (!tz || tz[0] == '\0') return;
    strncpy(s_clock.tz, tz, sizeof(s_clock.tz) - 1);
    s_clock.tz[sizeof(s_clock.tz) - 1] = '\0';
    apply_timezone();
    fire_immediate_tick();
    ESP_LOGI(TAG, "Timezone → %s", s_clock.tz);
}

void clock_set_format_24h(bool fmt24)
{
    s_clock.fmt24 = fmt24;
    fire_immediate_tick();
    ESP_LOGI(TAG, "Format → %s", fmt24 ? "24h" : "12h");
}

int clock_get_hour(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return s_clock.fmt24 ? tm.tm_hour
                   : (tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12);
}

int clock_get_minute(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_min;
}

int clock_get_second(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_sec;
}

bool clock_is_synced(void)
{
    return s_clock.synced;
}

void clock_sync_sntp(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync...");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");   /* Aliyun NTP — reachable in China */
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
}
