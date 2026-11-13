/*
 * NomadCast — System Clock
 *
 * Wall-clock time service backed by ESP32-S3 RTC timer.
 * Auto SNTP sync when WiFi connects. Applies timezone + 12h/24h format.
 *
 * Time updates are published via app_event: APP_EVENT_CLOCK_TICK (1/min).
 * Listeners call app_event_register() — no direct callback API needed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Initialize the clock. Creates the 1-second LVGL update timer. */
void clock_init(void);

/* ── Configuration (called by Settings model) ───────────────────────────── */

/** Set timezone from a POSIX TZ string (e.g. "CST-8" = UTC+8). */
void clock_set_timezone(const char *tz);

/** Set 12h (false) or 24h (true) format. */
void clock_set_format_24h(bool fmt24);

/* ── Current time ───────────────────────────────────────────────────────── */

int  clock_get_hour(void);
int  clock_get_minute(void);
int  clock_get_second(void);
bool clock_is_synced(void);

/* ── SNTP ───────────────────────────────────────────────────────────────── */

void clock_sync_sntp(void);

#ifdef __cplusplus
}
#endif
