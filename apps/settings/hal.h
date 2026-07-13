/*
 * HAL declarations for Settings app (ESP-IDF target).
 *
 * PC demo overrides these in pc_demo/hal.h + pc_demo/hal.c.
 * On ESP32-S3, hal_esp.c provides real WiFi via esp_wifi.
 */

#ifndef SETTINGS_HAL_H
#define SETTINGS_HAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── WiFi scan result ───────────────────────────────────────────────────── */
/* Mirrors WifiNetwork in model.h — kept separate so hal is self-contained. */

typedef struct {
    char    ssid[32];
    int     signal_strength;   /* 0–100, mapped from RSSI */
    bool    secured;           /* true if auth != OPEN */
    uint8_t bssid[6];          /* BSSID from scan — for direct connect */
    uint8_t channel;           /* primary channel */
} hal_wifi_ap_t;

/* ── WiFi lifecycle ─────────────────────────────────────────────────────── */

/** Init WiFi stack (netif + event loop + STA start). Safe to call multiple times. */
bool hal_wifi_init(void);

/** Stop and deinit WiFi. */
void hal_wifi_deinit(void);

/* ── Current connection ─────────────────────────────────────────────────── */

/** Check if WiFi STA is currently connected to an AP. */
bool hal_wifi_is_connected(void);

/** Get the SSID of the currently connected AP. Returns false if not connected. */
bool hal_wifi_get_current_ssid(char *ssid_out, int size);

/* ── WiFi scan ──────────────────────────────────────────────────────────── */

/**
 * @brief Blocking WiFi scan (~1–3 s). Allocates result array.
 * @param out  Receives malloc'd array — caller must free(*out).
 * @return Number of APs found, 0 on failure or no APs.
 */
int hal_wifi_scan(hal_wifi_ap_t **out);

/* ── WiFi connect ───────────────────────────────────────────────────────── */

/**
 * @brief Connect to AP — blocks until success, failure, or ~15 s timeout.
 * After return, result is available via hal_wifi_get_connect_result().
 */
void hal_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Connect with explicit BSSID + channel from a previous scan.
 * Avoids the driver's internal scan-cache lookup which can race after
 * a fresh scan and cause "Haven't to connect to a suitable AP now!".
 */
void hal_wifi_connect_bssid(const char *ssid, const char *password,
                             const uint8_t bssid[6], uint8_t channel);

/**
 * @brief Poll connect result (one-shot — consumed on first call).
 * @return true if a result is available, false if nothing pending.
 */
bool hal_wifi_get_connect_result(bool *out_success, const char **out_error);

/** Get raw WiFi disconnect reason code (e.g. 15=4WAY_HANDSHAKE, 202=AUTH_FAIL) */
int  hal_wifi_get_disconnect_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_HAL_H */
