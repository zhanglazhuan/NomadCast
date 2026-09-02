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

/** Set timezone by index: 0=UTC+8, 1=UTC+0, 2=UTC-5, 3=UTC+9. */
void clock_set_timezone(int tz_idx);

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
