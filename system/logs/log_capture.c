/**
 * @file log_capture.c
 * @brief Auto-capture layer — hooks app_event bus for zero-effort logging.
 */

#include "log_system.h"
#include "log_uploader.h"
#include "app_event.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "log_cap";

/* ── Event → category/name mapping ──────────────────────────────────────── */

static void on_app_event(app_event_t event, const void *data)
{
    switch (event) {

    case APP_EVENT_WIFI_CONNECTED:
        log_event("net", "wifi_connected", NULL);
        log_uploader_on_wifi_connected();  /* trigger upload */
        break;

    case APP_EVENT_WIFI_DISCONNECTED:
        log_event("net", "wifi_disconnected", NULL);
        break;

    case APP_EVENT_DOWNLOAD_COMPLETED:
        log_event("sys", "download_completed", NULL);
        break;

    case APP_EVENT_DOWNLOAD_CHANGED:
        log_event("sys", "download_changed", NULL);
        break;

    case APP_EVENT_BATTERY_CHANGED: {
        const app_event_battery_t *b = (const app_event_battery_t *)data;
        if (b) {
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"pct\":%d,\"chg\":%s}",
                     b->percent, b->charging ? "true" : "false");
            log_event("sys", "battery", buf);
        }
        break;
    }

    case APP_EVENT_KEY_PLAY_PAUSE:
        log_event("ui", "key_play_pause", NULL);
        break;

    case APP_EVENT_KEY_VOL_UP:
        log_event("ui", "key_vol_up", NULL);
        break;

    case APP_EVENT_KEY_VOL_DOWN:
        log_event("ui", "key_vol_down", NULL);
        break;

    case APP_EVENT_CLOCK_TICK:
        /* Too frequent — skip. Session duration is captured via
         * session_start/session_end timestamps on the server side. */
        break;

    default:
        break;
    }
}

/* ── Public ──────────────────────────────────────────────────────────────── */

void log_capture_init(void)
{
    app_event_register(on_app_event);
    ESP_LOGI(TAG, "auto-capture registered (app_event listener)");
}
