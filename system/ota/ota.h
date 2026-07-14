/*
 * NomadCast — WiFi OTA Firmware Update
 *
 * Flow: manifest.json check → version compare → user confirm → download → flash → reboot
 *
 * Manifest format (JSON, hosted at any HTTPS URL):
 *   {"version":"1.2.0","url":"https://.../firmware.bin","size":720000,"changelog":"..."}
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Types ──────────────────────────────────────────────────────────────── */

/** Result of ota_check(). */
typedef enum {
    OTA_CHECK_UP_TO_DATE,        /* Same or newer version already running */
    OTA_CHECK_UPDATE_AVAILABLE,  /* Remote version > running version */
    OTA_CHECK_ERROR_NETWORK,     /* HTTP / DNS / TLS failure */
    OTA_CHECK_ERROR_PARSE,       /* manifest.json parse error */
    OTA_CHECK_ERROR_NO_URL,      /* Manifest URL not configured */
} ota_check_result_t;

typedef void (*ota_check_cb_t)(ota_check_result_t result,
                               const char *new_version,
                               const char *changelog,
                               const char *firmware_url,
                               void *user_data);

typedef void (*ota_progress_cb_t)(int percent, void *user_data);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** Confirm current firmware is valid (cancel rollback). Call once at boot. */
void ota_init(void);

/* ── Update check ───────────────────────────────────────────────────────── */

/**
 * @brief Check for firmware update.
 *
 * Downloads manifest.json from the given URL, compares the "version" field
 * against the running firmware, and calls cb with the result.
 *
 * @param manifest_url  Full URL to manifest.json (HTTPS or HTTP).
 * @param cb            Called when check completes (from main task context).
 * @param user_data     Passed through to cb.
 */
void ota_check(const char *manifest_url, ota_check_cb_t cb, void *user_data);

/* ── Download & install ─────────────────────────────────────────────────── */

/**
 * @brief Download and install firmware from the given URL.
 *
 * Erases the next OTA slot, downloads, writes, validates, sets boot partition.
 * Call esp_restart() after this returns successfully.
 *
 * @param firmware_url  Full URL to the .bin file.
 * @param progress_cb   Called with percentage 0-100 (NULL for no feedback).
 * @param user_data     Passed through to progress_cb.
 * @return true on success, false on failure.
 */
bool ota_perform(const char *firmware_url, ota_progress_cb_t progress_cb, void *user_data);

#ifdef __cplusplus
}
#endif
