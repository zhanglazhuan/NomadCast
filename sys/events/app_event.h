/**
 * @file app_event.h
 * @brief Application-level event bus — sys/events component.
 *
 * Events carry optional typed data. Fire from anywhere, listen from anywhere.
 */

#ifndef APP_EVENT_H
#define APP_EVENT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event types ────────────────────────────────────────────────────────── */

typedef enum {
    APP_EVENT_CLOCK_TICK,            /* data: app_event_clock_tick_t* */
    APP_EVENT_WIFI_CONNECTED,        /* data: NULL */
    APP_EVENT_WIFI_DISCONNECTED,     /* data: NULL */
    APP_EVENT_DOWNLOAD_COMPLETED,    /* data: NULL — a download task finished */
    APP_EVENT_DOWNLOAD_CHANGED,      /* data: NULL — a task changed state w/o success (e.g. failed) */
    APP_EVENT_BATTERY_CHANGED,       /* data: app_event_battery_t* */
    APP_EVENT_KEY_PLAY_PAUSE,        /* data: NULL — physical play/pause key */
    APP_EVENT_KEY_VOL_UP,            /* data: NULL — physical volume-up key */
    APP_EVENT_KEY_VOL_DOWN,          /* data: NULL — physical volume-down key */
} app_event_t;

/* ── Event data structs ─────────────────────────────────────────────────── */

/** Payload for APP_EVENT_CLOCK_TICK */
typedef struct {
    int  hour;
    int  minute;
    bool synced;
} app_event_clock_tick_t;

/** Payload for APP_EVENT_BATTERY_CHANGED */
typedef struct {
    int  percent;    /* 0..100 */
    bool charging;
} app_event_battery_t;

/* ── Callback ───────────────────────────────────────────────────────────── */

typedef void (*app_event_cb_t)(app_event_t event, const void *data);

/* ── API ────────────────────────────────────────────────────────────────── */

void app_event_register(app_event_cb_t cb);
void app_event_fire(app_event_t event, const void *data);

#ifdef __cplusplus
}
#endif

#endif
