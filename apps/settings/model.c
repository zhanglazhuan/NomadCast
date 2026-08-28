#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "hal.h"
#include "wifi_cred.h"
#include "esp_log.h"

static const char *TAG = "settings";
#include "app.h"
#include "flash_store.h"
#include "sleep_monitor.h"
#include "clock.h"

/* The scan result (hal_wifi_ap_t[]) is cast straight to WifiNetwork* below.
 * Guard the assumption at compile time — a size mismatch silently garbles the
 * SSID of every network after the first (wrong indexing stride). */
_Static_assert(sizeof(WifiNetwork) == sizeof(hal_wifi_ap_t),
               "WifiNetwork and hal_wifi_ap_t must have identical layout");

/* ── 预定义选项 ────────────────────────────────────────────────────────────── */

const char* settings_timezone_labels[] = {"UTC+8 Beijing", "UTC+0 London", "UTC-5 New York", "UTC+9 Tokyo"};
const char* settings_timezone_options = "UTC+8 Beijing\nUTC+0 London\nUTC-5 New York\nUTC+9 Tokyo";

const char* settings_language_labels[] = {"简体中文", "English"};
const char* settings_language_options = "简体中文\nEnglish";

/* ── 生命周期 ──────────────────────────────────────────────────────────────── */

void settings_model_init(struct SettingsApp* app) {
    app->model = (SettingsModel*)malloc(sizeof(SettingsModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model memory allocation failed");
        return;
    }
    memset(app->model, 0, sizeof(SettingsModel));

    // Defaults (overridden by NVS if saved values exist)
    app->model->timezone_idx = flash_get_i32("settings", "tz", 0);
    app->model->language_idx = flash_get_i32("settings", "lang", 0);
    app->model->time_format_24h = flash_get_bool("settings", "fmt24", true);
    app->model->sleep_timeout_min = flash_get_i32("settings", "sleep", 5);
    app->model->auto_power_off_min = flash_get_i32("settings", "auto_power_off", 15);
    app->model->wifi_enabled = flash_get_bool("settings", "wifi", true);
    /* Restore WiFi state from hardware (survives app exit/re-enter) */
    app->model->connected_ssid[0] = '\0';
    app->model->scanned_networks = NULL;
    app->model->scanned_count = 0;
    app->model->wifi_scanning = false;
    app->model->wifi_scan_autoconnect = false;
    app->model->auto_update = flash_get_bool("settings", "autoup", false);
    app->model->update_checking = false;

    /* Sync sleep monitor with loaded timeouts */
    sleep_monitor_set_timeout(app->model->sleep_timeout_min);
    sleep_monitor_set_auto_power_off_timeout(app->model->auto_power_off_min);

    /* Sync clock with loaded settings */
    clock_set_timezone(app->model->timezone_idx);
    clock_set_format_24h(app->model->time_format_24h);

    /* Restore WiFi state from hardware (survives app exit/re-enter) */
    if (app->model->wifi_enabled) {
        hal_wifi_init();  /* idempotent — no-op if already running */

        /* Restore connected SSID from live hardware state */
        char cur_ssid[32];
        if (hal_wifi_get_current_ssid(cur_ssid, sizeof(cur_ssid))) {
            strncpy(app->model->connected_ssid, cur_ssid,
                    sizeof(app->model->connected_ssid) - 1);
            ESP_LOGI(TAG, "Restored connected SSID: '%s'", cur_ssid);
        }

        /* Scan so the network list is populated without user action */
        if (app->model->scanned_count == 0) {
            hal_wifi_ap_t *nets = NULL;
            int count = hal_wifi_scan(&nets);
            app->model->scanned_networks = (WifiNetwork *)nets;
            app->model->scanned_count    = count;
        }
    }

    ESP_LOGI(TAG, "init done");
}

void settings_model_deinit(struct SettingsApp* app) {
    if (app->model) {
        if (app->model->scanned_networks) free(app->model->scanned_networks);
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}

/* ── General ───────────────────────────────────────────────────────────────── */

int settings_model_get_timezone(struct SettingsApp* app) {
    return app->model ? app->model->timezone_idx : 0;
}
void settings_model_set_timezone(struct SettingsApp* app, int idx) {
    if (!app->model) return;
    app->model->timezone_idx = idx;
    flash_set_i32("settings", "tz", idx);
    clock_set_timezone(idx);
    ESP_LOGI(TAG, "timezone set to %d",idx);
}

int settings_model_get_language(struct SettingsApp* app) {
    return app->model ? app->model->language_idx : 0;
}
void settings_model_set_language(struct SettingsApp* app, int idx) {
    if (!app->model) return;
    app->model->language_idx = idx;
    flash_set_i32("settings", "lang", idx);
    ESP_LOGI(TAG, "language set to %d",idx);
}

bool settings_model_get_time_format_24h(struct SettingsApp* app) {
    return app->model ? app->model->time_format_24h : true;
}
void settings_model_set_time_format_24h(struct SettingsApp* app, bool fmt24) {
    if (!app->model) return;
    app->model->time_format_24h = fmt24;
    flash_set_bool("settings", "fmt24", fmt24);
    clock_set_format_24h(fmt24);
    ESP_LOGI(TAG, "time format 24h=%d",fmt24);
}

int settings_model_get_sleep_timeout(struct SettingsApp* app) {
    return app->model ? app->model->sleep_timeout_min : 5;
}
void settings_model_set_sleep_timeout(struct SettingsApp* app, int minutes) {
    if (!app->model) return;
    app->model->sleep_timeout_min = minutes;
    flash_set_i32("settings", "sleep", minutes);
    sleep_monitor_set_timeout(minutes);
    ESP_LOGI(TAG, "sleep timeout set to %d min",minutes);
}

int settings_model_get_auto_power_off(struct SettingsApp* app) {
    return app->model ? app->model->auto_power_off_min : 15;
}
void settings_model_set_auto_power_off(struct SettingsApp* app, int minutes) {
    if (!app->model) return;
    app->model->auto_power_off_min = minutes;
    flash_set_i32("settings", "auto_power_off", minutes);
    sleep_monitor_set_auto_power_off_timeout(minutes);
    ESP_LOGI(TAG, "auto power-off set to %d min", minutes);
}

/* ── WIFI ──────────────────────────────────────────────────────────────────── */

bool settings_model_get_wifi_enabled(struct SettingsApp* app) {
    return app->model ? app->model->wifi_enabled : false;
}
void settings_model_set_wifi_enabled(struct SettingsApp* app, bool enabled) {
    if (!app->model) return;
    app->model->wifi_enabled = enabled;
    flash_set_bool("settings", "wifi", enabled);
    ESP_LOGI(TAG, "wifi enabled=%d",enabled);

    if (enabled) {
        /* Init WiFi hardware only (fast, non-blocking).
         * Scan + auto-connect runs async via settings_model_do_scan_and_connect(). */
        hal_wifi_init();
        app->model->wifi_scanning = true;
        app->model->wifi_scan_autoconnect = true;  /* enabling WiFi → scan + auto-connect */
    } else {
        app->model->connected_ssid[0] = '\0';
        if (app->model->scanned_networks) free(app->model->scanned_networks);
        app->model->scanned_networks = NULL;
        app->model->scanned_count = 0;
        hal_wifi_deinit();
        /* APP_EVENT_WIFI_DISCONNECTED fired by esp_wifi_stop → event handler */
    }
}

/**
 * @brief Async scan + auto-connect. Called from a timer after WiFi init,
 *        so the UI switch responds instantly and this work runs in the background.
 */
void settings_model_do_scan_and_connect(struct SettingsApp *app)
{
    if (!app->model || !app->model->wifi_enabled) return;

    ESP_LOGI(TAG, "Async scan + connect starting…");

    /* 1. Scan */
    hal_wifi_ap_t *nets = NULL;
    int count = hal_wifi_scan(&nets);
    app->model->scanned_networks = (WifiNetwork *)nets;
    app->model->scanned_count    = count;

    /* 2. Auto-connect: try saved credentials newest-first */
    wifi_cred_t creds[MAX_WIFI_CREDS];
    int ncreds = wifi_cred_load_all(creds, MAX_WIFI_CREDS);
    for (int i = 0; i < ncreds; i++) {
        bool in_range = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(nets[j].ssid, creds[i].ssid) == 0) {
                in_range = true; break;
            }
        }
        if (!in_range) continue;

        ESP_LOGI(TAG, "Auto-connecting to '%s'…", creds[i].ssid);
        hal_wifi_connect(creds[i].ssid, creds[i].password);
        bool ok = false;
        const char *err = NULL;
        hal_wifi_get_connect_result(&ok, &err);
        if (ok) {
            strncpy(app->model->connected_ssid, creds[i].ssid,
                    sizeof(app->model->connected_ssid) - 1);
            wifi_cred_save(creds[i].ssid, creds[i].password);
            ESP_LOGI(TAG, "Auto-connected to '%s'", creds[i].ssid);
            break;
        } else {
            int reason = hal_wifi_get_disconnect_reason();
            ESP_LOGW(TAG, "Auto-connect to '%s' failed: %s (reason %d)",
                     creds[i].ssid, err ? err : "?", reason);
            /* Only delete credential on password-related errors.
             * Transient failures (weak signal, AP congestion, timeout)
             * should NOT purge the saved password. */
            if (reason == 2 || reason == 202) {
                ESP_LOGI(TAG, "Deleting credential for '%s' (password error)", creds[i].ssid);
                wifi_cred_delete(creds[i].ssid);
            }
        }
    }

    app->model->wifi_scanning = false;
    ESP_LOGI(TAG, "Async scan + connect done");
}

const char* settings_model_get_connected_ssid(struct SettingsApp* app) {
    if (!app->model || !app->model->wifi_enabled) return NULL;
    return app->model->connected_ssid[0] ? app->model->connected_ssid : NULL;
}

int settings_model_get_scanned_count(struct SettingsApp* app) {
    return app->model ? app->model->scanned_count : 0;
}

const WifiNetwork* settings_model_get_scanned_networks(struct SettingsApp* app) {
    return app->model ? app->model->scanned_networks : NULL;
}

void settings_model_request_wifi_scan(struct SettingsApp* app) {
    if (!app->model || !app->model->wifi_enabled) return;

    /* Ensure WiFi hardware is initialized (idempotent) */
    hal_wifi_init();

    /* Clear the current list so the view renders an empty "Scanning..." state.
     * The actual (blocking) scan runs afterwards from the view's timer via
     * settings_model_start_wifi_scan(). */
    if (app->model->scanned_networks) {
        free(app->model->scanned_networks);
        app->model->scanned_networks = NULL;
    }
    app->model->scanned_count = 0;
    app->model->wifi_scanning = true;
    app->model->wifi_scan_autoconnect = false;  /* manual scan: list only, don't touch connection */
}

void settings_model_start_wifi_scan(struct SettingsApp* app) {
    if (!app->model || !app->model->wifi_enabled) return;

    /* Ensure WiFi hardware is initialized (idempotent) */
    hal_wifi_init();

    /* Free old results */
    if (app->model->scanned_networks) {
        free(app->model->scanned_networks);
        app->model->scanned_networks = NULL;
    }
    app->model->scanned_count = 0;
    app->model->wifi_scanning = true;

    /* Real blocking scan (~1-3 s) */
    hal_wifi_ap_t *nets = NULL;
    int count = hal_wifi_scan(&nets);

    /* hal_wifi_ap_t and WifiNetwork have identical binary layout — safe cast */
    app->model->scanned_networks = (WifiNetwork *)nets;
    app->model->scanned_count    = count;
    app->model->wifi_scanning    = false;
    ESP_LOGI(TAG, "wifi scan complete, %d networks found", count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  [%d] \"%s\"  RSSI=%d dBm",
                 i + 1, nets[i].ssid, nets[i].rssi);
    }
}

void settings_model_set_pending_connect(struct SettingsApp* app, const char* ssid) {
    if (!app->model) return;
    app->model->pending_connect = true;
    strncpy(app->model->pending_ssid, ssid, sizeof(app->model->pending_ssid) - 1);
    app->model->pending_ssid[sizeof(app->model->pending_ssid) - 1] = '\0';
}

bool settings_model_auto_connect(struct SettingsApp* app, const char *ssid)
{
    if (!app->model || !app->model->wifi_enabled) return false;

    /* Look up saved password */
    char saved_pwd[64];
    if (!wifi_cred_find(ssid, saved_pwd, sizeof(saved_pwd)))
        return false;

    ESP_LOGI(TAG, "Auto-connecting to '%s' with saved password…", ssid);
    hal_wifi_connect(ssid, saved_pwd);

    bool ok = false;
    const char *err = NULL;
    hal_wifi_get_connect_result(&ok, &err);

    if (ok) {
        strncpy(app->model->connected_ssid, ssid,
                sizeof(app->model->connected_ssid) - 1);
        wifi_cred_save(ssid, saved_pwd);
        ESP_LOGI(TAG, "Auto-connected to '%s'", ssid);
        return true;
    } else {
        int reason = hal_wifi_get_disconnect_reason();
        ESP_LOGW(TAG, "Auto-connect to '%s' failed: %s (reason %d)",
                 ssid, err ? err : "?", reason);
        /* Only delete credential on password-related errors (2, 202).
         * Transient failures should not purge the saved password. */
        if (reason == 2 || reason == 202) {
            ESP_LOGI(TAG, "Deleting credential for '%s' (password error)", ssid);
            wifi_cred_delete(ssid);
        }
        return false;
    }
}

bool settings_model_get_and_clear_pending_connect(struct SettingsApp* app, char* ssid_out, int ssid_size) {
    if (!app->model || !app->model->pending_connect) return false;
    if (ssid_out) strncpy(ssid_out, app->model->pending_ssid, ssid_size - 1);
    app->model->pending_connect = false;
    return true;
}

/* ── Storage ───────────────────────────────────────────────────────────────── */

#include "ff.h"  /* FatFS f_getfree */

int settings_model_get_storage_used_mb(struct SettingsApp* app) {
    (void)app;
    FATFS *fs = NULL;
    DWORD free_clst = 0;
    /* FatFS drive "0:" = first mounted volume (SD card at /sdcard) */
    FRESULT res = f_getfree("0:/", &free_clst, &fs);
    if (res != FR_OK || !fs) return -1;
    DWORD total_sectors = (fs->n_fatent - 2) * fs->csize;
    DWORD used_sectors  = total_sectors - free_clst * fs->csize;
    uint64_t used_bytes  = (uint64_t)used_sectors  * fs->ssize;
    return (int)(used_bytes / (1024 * 1024));
}

int settings_model_get_storage_total_mb(struct SettingsApp* app) {
    (void)app;
    FATFS *fs = NULL;
    DWORD free_clst = 0;
    FRESULT res = f_getfree("0:/", &free_clst, &fs);
    if (res != FR_OK || !fs) return -1;
    uint64_t total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize;
    return (int)(total_bytes / (1024 * 1024));
}

void settings_model_clean_storage(struct SettingsApp* app) {
    (void)app;
    /* TODO: implement actual cleanup (delete .nomadcast/downloads/ etc.) */
    ESP_LOGI(TAG, "storage clean requested (not yet implemented)");
}

/* ── Update ─────────────────────────────────────────────────────────────────── */

bool settings_model_get_auto_update(struct SettingsApp* app) {
    return app->model ? app->model->auto_update : false;
}
void settings_model_set_auto_update(struct SettingsApp* app, bool enabled) {
    if (!app->model) return;
    app->model->auto_update = enabled;
    flash_set_bool("settings", "autoup", enabled);
    ESP_LOGI(TAG, "auto update set to %d",enabled);
}
void settings_model_check_update(struct SettingsApp* app) {
    if (!app->model) return;
    app->model->update_checking = true;
    ESP_LOGI(TAG, "checking for update...");
    // Mock: 1 秒后自动设为 false (在 view 层用 timer 模拟)
}
