/*
 * WiFi Manager — Public API
 *
 * Complete WiFi STA management for ESP32-S3.
 *
 * State machine:
 *   wifi_init()     → subsystem ready (radio OFF)
 *   wifi_start()    → radio ON
 *   wifi_stop()     → radio OFF (low power, credentials preserved)
 *   wifi_connect()  → connect + block until IP obtained
 *   wifi_disconnect() → manual disconnect
 *   wifi_reconnect()  → reconnect using last saved credentials
 *   wifi_deinit()   → full teardown
 *
 * All blocking functions use FreeRTOS event groups internally;
 * only the calling task blocks.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Types
 * ======================================================================== */

/** Connection state snapshot. */
typedef struct {
    char            ssid[33];
    char            ip[16];
    char            bssid[18];
    int8_t          rssi;
    uint8_t         channel;
    bool            is_connected;
    wifi_auth_mode_t auth_mode;
} wifi_info_t;

typedef void (*wifi_connected_cb_t)(const wifi_info_t *info);
typedef void (*wifi_disconnected_cb_t)(void);

/* ========================================================================
 * 1. Subsystem Lifecycle
 * ======================================================================== */

/** One-time init (NVS, TCP/IP, event loop, netif, wifi driver).  Radio is
 *  NOT started yet — call wifi_start() or wifi_connect() to turn it on. */
esp_err_t wifi_init(void);

/** Full teardown.  Stops radio, unregisters handlers, frees wifi/network
 *  resources.  wifi_init() must be called again before any other use. */
esp_err_t wifi_deinit(void);

/* ========================================================================
 * 2. Radio Control (Power)
 * ======================================================================== */

/** Turn WiFi radio ON.  Must call wifi_init() first.  Safe to call when
 *  already on (no-op). */
esp_err_t wifi_start(void);

/** Turn WiFi radio OFF — saves power while preserving:
 *    - saved credentials (wifi_reconnect() still works)
 *    - TCP/IP stack and event loop
 *    - all registered callbacks
 *  Must call wifi_init() first. */
esp_err_t wifi_stop(void);

/* ========================================================================
 * 3. Connection
 * ======================================================================== */

/** Connect to an AP.  Blocks until IP obtained or timeout (15 s).
 *  If radio is off, starts it automatically.  Credentials are saved
 *  for wifi_reconnect(). */
esp_err_t wifi_connect(const char *ssid, const char *password);

/** Reconnect using the last successfully used credentials
 *  (from wifi_connect() or wifi_set_config()). */
esp_err_t wifi_reconnect(void);

/** Disconnect from the current AP. */
esp_err_t wifi_disconnect(void);

/* ========================================================================
 * 4. Scan
 * ======================================================================== */

/** Scan for nearby APs (blocking).  If connected, temporarily
 *  disconnects → scans all channels → reconnects automatically.
 *
 * @param records      Pre-allocated array for results.
 * @param max_count    Size of @p records.
 * @param[out] found   Number of APs written to @p records.
 * @param timeout_ms   Scan timeout (0 = 10 s default). */
esp_err_t wifi_scan(wifi_ap_record_t *records, uint16_t max_count,
                    uint16_t *found, uint32_t timeout_ms);

/* ========================================================================
 * 5. Status
 * ======================================================================== */

/** Get current connection snapshot.  RSSI/channel are refreshed live;
 *  IP/SSID/BSSID are cached from the last GOT_IP event. */
esp_err_t wifi_get_info(wifi_info_t *info);

/** Quick check — prefer over wifi_get_info() when you only need a bool. */
bool wifi_is_connected(void);

/* ========================================================================
 * 6. Credential Persistence
 * ======================================================================== */

/** Read the last-saved credentials (from wifi_connect() or wifi_set_config()).
 *  Does NOT read the live WiFi config register — reads RAM cache. */
esp_err_t wifi_get_config(char *ssid, size_t ssid_len,
                          char *password, size_t pwd_len);

/** Store credentials for later reconnect.  Does NOT connect. */
esp_err_t wifi_set_config(const char *ssid, const char *password);

/* ========================================================================
 * 7. Power Management
 * ======================================================================== */

/** Set WiFi power-save mode.
 *  WIFI_PS_NONE       — always on, lowest latency, highest power
 *  WIFI_PS_MIN_MODEM  — DTIM-based sleep (balanced)
 *  WIFI_PS_MAX_MODEM  — aggressive sleep (best battery, higher latency) */
esp_err_t wifi_set_power_save(wifi_ps_type_t mode);

/* ========================================================================
 * 8. Event Callbacks
 * ======================================================================== */

/** Register callbacks for connection state changes.  Pass NULL to clear. */
void wifi_set_callbacks(wifi_connected_cb_t on_connected,
                        wifi_disconnected_cb_t on_disconnected);

#ifdef __cplusplus
}
#endif

#endif
