/**
 * @file log_system.h
 * @brief Session-based logging system — SD card + WiFi upload.
 *
 * Lifecycle:
 *   log_system_init()     — after SD mount, starts new session
 *   log_system_shutdown() — before sleep, ends session
 *
 * Log entries are one JSON object per line, flushed to SD periodically.
 * Upload is handled automatically when WiFi connects (low-priority task).
 */

#ifndef LOG_SYSTEM_H
#define LOG_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Initialize the logging system. SD card must already be mounted at /sdcard.
 *  Creates session file, starts flush timer, registers event capture. */
void log_system_init(void);

/** End current session — flush remaining buffer, close session file.
 *  Called automatically by pre-sleep callback. Safe to call multiple times. */
void log_system_shutdown(void);

/* ── Manual logging (for future explicit instrumentation) ───────────────── */

/** Log a user-facing event.
 *  @param category  One of: sys, ui, net, audio, error, metric
 *  @param event     Event name (e.g. "boot", "app_open", "wifi_connected")
 *  @param data_json JSON object string, or NULL for empty {} */
void log_event(const char *category, const char *event, const char *data_json);

/* ── Page navigation hook ───────────────────────────────────────────────── */

/** Called by page_navigator when a page is pushed or popped.
 *  @param direction  "push" or "pop"
 *  @param page_name  Human-readable page name (e.g. "wifi_connect") */
void log_page_nav(const char *direction, const char *page_name);

#ifdef __cplusplus
}
#endif

#endif
