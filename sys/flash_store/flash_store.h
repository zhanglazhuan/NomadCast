/*
 * NomadCast — Flash Store (NVS-backed key-value persistence)
 *
 * Lightweight wrapper around ESP-IDF NVS. Each app/module uses its own
 * namespace to avoid key collisions.
 *
 * Usage:
 *   flash_store_init();                                     // once in app_main()
 *   int tz = flash_get_i32("settings", "tz", 0);            // read, default 0
 *   flash_set_i32("settings", "tz", 3);                     // write
 *   flash_erase_ns("settings");                             // factory-reset one app
 *   flash_erase_all();                                      // full factory reset
 *
 * Namespace convention (max 15 chars per NVS limit):
 *   "settings"  — Settings app (general, wifi, storage, update)
 *   "podcast"   — Podcast app (font, quality, login, history)
 *   "sleep"     — Sleep monitor timeout
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Initialize NVS. Safe to call multiple times (idempotent). */
void flash_store_init(void);

/* ── Int32 ──────────────────────────────────────────────────────────────── */

int32_t flash_get_i32(const char *ns, const char *key, int32_t def);
void    flash_set_i32(const char *ns, const char *key, int32_t val);

/* ── Bool (stored as i32: 0/1) ──────────────────────────────────────────── */

bool    flash_get_bool(const char *ns, const char *key, bool def);
void    flash_set_bool(const char *ns, const char *key, bool val);

/* ── String ─────────────────────────────────────────────────────────────── */

/**
 * @brief Read a string. Returns strlen of the value read (0 if not found).
 * @param out  Output buffer.
 * @param max  Buffer size.
 * @param def  Default value if key not found.
 */
int     flash_get_str(const char *ns, const char *key, char *out, size_t max, const char *def);
void    flash_set_str(const char *ns, const char *key, const char *val);

/* ── Erase ──────────────────────────────────────────────────────────────── */

/** Erase all keys in one namespace. */
void    flash_erase_ns(const char *ns);

/** Erase the entire NVS partition (full factory reset). Requires reboot. */
void    flash_erase_all(void);

#ifdef __cplusplus
}
#endif
