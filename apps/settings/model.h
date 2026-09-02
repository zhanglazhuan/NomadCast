#ifndef SETTINGS_MODEL_H
#define SETTINGS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

struct SettingsApp;

/* ── WiFi 扫描结果 ─────────────────────────────────────────────────────────── */

/* MUST stay binary-identical to hal_wifi_ap_t (hal.h): model.c casts the
 * hal_wifi_scan() result array (hal_wifi_ap_t[]) straight to WifiNetwork*.
 * If the two ever differ in size, indexing nets[i] reads at the wrong stride
 * and every entry after the first gets a garbled SSID. */
typedef struct {
    char    ssid[32];
    int     signal_strength;  // 0-100
    bool    secured;
    uint8_t bssid[6];         // BSSID from scan — for direct/BSSID connect
    uint8_t channel;          // primary channel
    int8_t  rssi;             // raw RSSI in dBm (e.g. -45)
} WifiNetwork;

/* ── 设置数据 ──────────────────────────────────────────────────────────────── */

typedef struct SettingsModel {
    // General
    int  timezone_idx;       // 0=UTC+8, 1=UTC+0, 2=UTC-5, ...
    int  language_idx;       // 0=CN, 1=EN, ...
    bool time_format_24h;    // true=24h, false=12h
    int  sleep_timeout_min;  // 0=never, 1/2/5/10/15/30/60 minutes
    int  auto_power_off_min; // 0=never, 5/10/15/30/60 minutes (default 15)

    // WIFI
    bool wifi_enabled;
    char connected_ssid[32];
    WifiNetwork* scanned_networks;
    int  scanned_count;
    bool wifi_scanning;
    bool wifi_scan_autoconnect;  // true: initial scan auto-connects; false: manual scan is list-only
    bool pending_connect;
    char pending_ssid[32];

    // Storage
    int storage_used_mb;
    int storage_total_mb;

    // Update
    bool auto_update;
    bool update_checking;
} SettingsModel;

/* ── 预定义选项 ────────────────────────────────────────────────────────────── */

#define SETTINGS_TIMEZONE_COUNT 4
extern const char* settings_timezone_labels[];
extern const char* settings_timezone_options;

#define SETTINGS_LANGUAGE_COUNT 2
extern const char* settings_language_labels[];
extern const char* settings_language_options;

/* ── 生命周期 ──────────────────────────────────────────────────────────────── */

void settings_model_init(struct SettingsApp* app);
void settings_model_deinit(struct SettingsApp* app);

/* ── General ───────────────────────────────────────────────────────────────── */

int  settings_model_get_timezone(struct SettingsApp* app);
void settings_model_set_timezone(struct SettingsApp* app, int idx);
int  settings_model_get_language(struct SettingsApp* app);
void settings_model_set_language(struct SettingsApp* app, int idx);
bool settings_model_get_time_format_24h(struct SettingsApp* app);
void settings_model_set_time_format_24h(struct SettingsApp* app, bool fmt24);
int  settings_model_get_sleep_timeout(struct SettingsApp* app);
void settings_model_set_sleep_timeout(struct SettingsApp* app, int minutes);
int  settings_model_get_auto_power_off(struct SettingsApp* app);
void settings_model_set_auto_power_off(struct SettingsApp* app, int minutes);

/* ── WIFI ──────────────────────────────────────────────────────────────────── */

bool settings_model_get_wifi_enabled(struct SettingsApp* app);
void settings_model_set_wifi_enabled(struct SettingsApp* app, bool enabled);
const char* settings_model_get_connected_ssid(struct SettingsApp* app);
int  settings_model_get_scanned_count(struct SettingsApp* app);
const WifiNetwork* settings_model_get_scanned_networks(struct SettingsApp* app);
void settings_model_start_wifi_scan(struct SettingsApp* app);
/** Mark a manual (list-only) scan as pending: clears results + sets scanning
 *  flag, WITHOUT blocking. The view renders "Scanning..." then a timer runs the
 *  actual scan via settings_model_start_wifi_scan(). */
void settings_model_request_wifi_scan(struct SettingsApp* app);

/**
 * @brief Async scan + auto-connect (blocking ~1-15 s). Call from an LVGL timer
 *        so the UI stays responsive.  Updates model in place — caller should
 *        refresh the page afterwards via page_navigator_navigate_to().
 */
void settings_model_do_scan_and_connect(struct SettingsApp *app);

void settings_model_set_pending_connect(struct SettingsApp* app, const char* ssid);
bool settings_model_get_and_clear_pending_connect(struct SettingsApp* app, char* ssid_out, int ssid_size);

/**
 * @brief Try to auto-connect to a network using saved credentials.
 *        If a saved password exists: connect. On success, bump recency.
 *        On failure, delete the saved credential.
 * @return true if connected successfully, false otherwise.
 */
bool settings_model_auto_connect(struct SettingsApp* app, const char *ssid);

/* ── Storage ───────────────────────────────────────────────────────────────── */

int settings_model_get_storage_used_mb(struct SettingsApp* app);
int settings_model_get_storage_total_mb(struct SettingsApp* app);
void settings_model_clean_storage(struct SettingsApp* app);

/* ── Update ─────────────────────────────────────────────────────────────────── */

bool settings_model_get_auto_update(struct SettingsApp* app);
void settings_model_set_auto_update(struct SettingsApp* app, bool enabled);
void settings_model_check_update(struct SettingsApp* app);

#endif // SETTINGS_MODEL_H
