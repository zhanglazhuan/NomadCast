/**
 * @file hal_esp.c
 * @brief ESP-IDF WiFi HAL — real esp_wifi init / scan / connect.
 *
 * Adapted from debug/t_wifi/main/t_wifi.c (proven working on ESP32-S3).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "hal.h"
#include "app_event.h"

static const char *TAG = "hal_wifi";

/* ── WiFi state ─────────────────────────────────────────────────────────── */

static bool            s_wifi_initialized = false;
static EventGroupHandle_t s_wifi_evt = NULL;
static esp_netif_t     *s_sta_netif = NULL;     /* saved for deinit cleanup */

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_TIMEOUT_MS     15000

/* Last connect result (consumed once by hal_wifi_get_connect_result) */
static bool s_has_result  = false;
static bool s_result_ok   = false;
static char s_result_err[64];

/* Captured disconnect reason (written by event handler, read by connect) */
static uint16_t s_disconnect_reason = 0;

/* ── Disconnect reason → human-readable string ─────────────────────────── */

static const char *wifi_reason_str(uint16_t reason)
{
    switch (reason) {
    case 1:   return "Unspecified";
    case 2:   return "Auth expired (transient)";
    case 3:   return "De-authenticated (leave)";
    case 4:   return "Association expired (transient)";
    case 15:  return "4-way handshake timeout (wrong password?)";
    case 201: return "No AP found";
    case 202: return "Auth failed (wrong password?)";
    case 203: return "Association failed";
    case 204: return "Handshake timeout";
    case 205: return "Connection failed";
    default:  return "Unknown error";
    }
}

/* ── Event handler (runs in WiFi task context) ──────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", d->reason);
        s_disconnect_reason = d->reason;
        app_event_fire(APP_EVENT_WIFI_DISCONNECTED, NULL);
        xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_disconnect_reason = 0;
        app_event_fire(APP_EVENT_WIFI_CONNECTED, NULL);
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

/* ── RSSI → 0..100 signal strength ──────────────────────────────────────── */

static int rssi_to_pct(int rssi)
{
    /* Typical WiFi RSSI range: -100 (worst) … -20 (excellent) */
    if (rssi >= -20)  return 100;
    if (rssi <= -100) return 0;
    return (rssi + 100) * 100 / 80;   /* -100→0, -20→100 */
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

bool hal_wifi_init(void)
{
    if (s_wifi_initialized) return true;

    ESP_LOGI(TAG, "Init WiFi stack…");

    /* Silence the ESP-IDF WiFi driver's INFO chatter (e.g. [ADDBA]RX DELBA
     * spam during heavy download traffic). Keep WARN/ERROR. Our own logs use
     * the "hal_wifi" tag and are unaffected. */
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* Safe to call multiple times — ESP-IDF guards internally */
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();  /* save handle for deinit */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    s_wifi_evt = xEventGroupCreate();
    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi init OK");
    return true;
}

void hal_wifi_deinit(void)
{
    if (!s_wifi_initialized) return;

    ESP_LOGI(TAG, "Deinit WiFi…");
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Destroy the netif so re-init works (avoids "duplicate key" assert) */
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_evt) {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    s_wifi_initialized = false;
    s_has_result = false;
    ESP_LOGI(TAG, "WiFi deinit done");
}

/* ── Current connection info ────────────────────────────────────────────── */

bool hal_wifi_is_connected(void)
{
    if (!s_wifi_initialized) return false;

    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool hal_wifi_get_current_ssid(char *ssid_out, int size)
{
    if (!s_wifi_initialized) return false;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;

    strncpy(ssid_out, (const char *)ap.ssid, size - 1);
    ssid_out[size - 1] = '\0';
    return true;
}

/* ── Scan ───────────────────────────────────────────────────────────────── */

int hal_wifi_scan(hal_wifi_ap_t **out)
{
    *out = NULL;

    if (!s_wifi_initialized) {
        ESP_LOGW(TAG, "Scan called before WiFi init");
        return 0;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid       = NULL,
        .bssid      = NULL,
        .channel    = 0,
        .show_hidden = false,
        .scan_type  = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_LOGI(TAG, "Starting scan…");
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true /* blocking */);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return 0;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGI(TAG, "No APs found");
        return 0;
    }

    wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * count);
    if (!aps) return 0;

    esp_wifi_scan_get_ap_records(&count, aps);

    /* Convert to HAL struct */
    hal_wifi_ap_t *nets = (hal_wifi_ap_t *)malloc(sizeof(hal_wifi_ap_t) * count);
    if (!nets) { free(aps); return 0; }

    for (int i = 0; i < count; i++) {
        strncpy(nets[i].ssid, (const char *)aps[i].ssid, sizeof(nets[i].ssid) - 1);
        nets[i].ssid[sizeof(nets[i].ssid) - 1] = '\0';
        if (nets[i].ssid[0] == '\0') {
            /* Hidden SSID — use placeholder */
            snprintf(nets[i].ssid, sizeof(nets[i].ssid), "<Hidden %d>", i);
        }
        nets[i].signal_strength = rssi_to_pct(aps[i].rssi);
        nets[i].rssi            = aps[i].rssi;
        nets[i].secured = (aps[i].authmode != WIFI_AUTH_OPEN);
        memcpy(nets[i].bssid, aps[i].bssid, 6);
        nets[i].channel = aps[i].primary;
    }

    free(aps);
    *out = nets;
    ESP_LOGI(TAG, "Scan done: %d APs", (int)count);
    return (int)count;
}

/* ── Connect ────────────────────────────────────────────────────────────── */

void hal_wifi_connect(const char *ssid, const char *password)
{
    if (!s_wifi_initialized) {
        ESP_LOGW(TAG, "Connect called before WiFi init");
        s_has_result = true;
        s_result_ok  = false;
        snprintf(s_result_err, sizeof(s_result_err), "WiFi not initialized");
        return;
    }

    /* Always reset the WiFi state machine before connecting.  A previous
     * failed auth attempt (reason 2) leaves the STA in an intermediate
     * state where esp_wifi_connect() reports "Haven't to connect to a
     * suitable AP now!" and fails again.  esp_wifi_disconnect() + a short
     * delay is the only reliable way back to the INIT state. */
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    /* Wait for the disconnect event (or timeout if already disconnected) */
    xEventGroupWaitBits(s_wifi_evt, WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Connecting to \"%s\"…", ssid);

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    /* Clear event bits before connecting */
    xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(ret));
        s_has_result = true;
        s_result_ok  = false;
        snprintf(s_result_err, sizeof(s_result_err), "Failed to start connection");
        return;
    }

    /* Block until connected, failed, or timeout */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,        /* don't clear on exit */
        pdFALSE,        /* wait for ANY bit */
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    s_has_result = true;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
        s_result_ok = true;
        s_result_err[0] = '\0';
    } else {
        const char *reason_str = wifi_reason_str(s_disconnect_reason);
        ESP_LOGW(TAG, "Connection failed: %s (reason %d)", reason_str, s_disconnect_reason);
        s_result_ok = false;
        snprintf(s_result_err, sizeof(s_result_err), "%s", reason_str);
    }
}

void hal_wifi_connect_bssid(const char *ssid, const char *password,
                             const uint8_t bssid[6], uint8_t channel)
{
    if (!s_wifi_initialized) {
        s_has_result = true; s_result_ok  = false;
        snprintf(s_result_err, sizeof(s_result_err), "WiFi not initialized");
        return;
    }

    if (hal_wifi_is_connected()) {
        ESP_LOGI(TAG, "Disconnecting current AP before connecting to \"%s\"", ssid);
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_wifi_evt, WIFI_FAIL_BIT,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "Connecting to \"%s\" (ch %d, "
             "%02x:%02x:%02x:%02x:%02x:%02x)…",
             ssid, channel, bssid[0],bssid[1],bssid[2],
             bssid[3],bssid[4],bssid[5]);

    wifi_config_t wifi_cfg = {
        .sta = { .channel = channel },
    };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    memcpy(wifi_cfg.sta.bssid, bssid, 6);
    /* Tell the driver to use the explicit BSSID, not just SSID */
    wifi_cfg.sta.bssid_set = 1;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(ret));
        s_has_result = true; s_result_ok  = false;
        snprintf(s_result_err, sizeof(s_result_err), "Failed to start connection");
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    s_has_result = true;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
        s_result_ok = true; s_result_err[0] = '\0';
    } else {
        const char *reason_str = wifi_reason_str(s_disconnect_reason);
        ESP_LOGW(TAG, "Connection failed: %s (reason %d)", reason_str, s_disconnect_reason);
        s_result_ok = false;
        snprintf(s_result_err, sizeof(s_result_err), "%s", reason_str);
    }
}

bool hal_wifi_get_connect_result(bool *out_success, const char **out_error)
{
    if (!s_has_result) return false;
    if (out_success) *out_success = s_result_ok;
    if (out_error)  *out_error  = s_result_ok ? NULL : s_result_err;
    return true;
}

int hal_wifi_get_disconnect_reason(void)
{
    return (int)s_disconnect_reason;
}
