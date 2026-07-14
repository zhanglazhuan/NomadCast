/*
 * WiFi Manager — ESP32-S3 Station Mode
 *
 * Complete WiFi STA API.  State machine:
 *
 *   UNINIT ──wifi_init()──▶ IDLE ──wifi_connect()──▶ CONNECTED
 *                              ▲                         │
 *                              │     wifi_disconnect()   │
 *                              └─────────────────────────┘
 *
 *   IDLE ──wifi_stop()──▶ STOPPED ──wifi_start()──▶ IDLE
 *
 *   ANY_STATE ──wifi_deinit()──▶ UNINIT
 *
 * All blocking calls use FreeRTOS event groups; only the calling
 * task blocks — background tasks continue running.
 */

#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WiFi";

/* ========================================================================
 * Internal State
 * ======================================================================== */

static bool                 s_initialized  = false;
static bool                 s_radio_on     = false;
static bool                 s_connected    = false;
static esp_netif_t         *s_netif_sta    = NULL;
static EventGroupHandle_t   s_event_group  = NULL;

/* Cached credentials for reconnect */
static char s_saved_ssid[33];
static char s_saved_password[65];

/* Event group bits */
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_DISCONNECTED_BIT  BIT1
#define WIFI_SCAN_DONE_BIT     BIT2
#define WIFI_STOPPED_BIT       BIT3

/* Timeouts (ms) */
#define WIFI_CONNECT_TIMEOUT_MS   15000
#define WIFI_SCAN_TIMEOUT_MS      10000

/* Cached connection info */
static wifi_info_t s_cached_info;

/* User callbacks */
static wifi_connected_cb_t    s_on_connected    = NULL;
static wifi_disconnected_cb_t s_on_disconnected = NULL;

/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data);
static void update_cached_info(void);
static void clear_cached_info(void);
static esp_err_t connect_internal(const char *ssid, const char *password);
static esp_err_t wait_for_connection(void);

/* ========================================================================
 * 1. Init / Deinit — subsystem lifecycle
 * ======================================================================== */

esp_err_t wifi_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi subsystem...");

    s_event_group = xEventGroupCreate();
    assert(s_event_group);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* TCP/IP + event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* STA netif */
    s_netif_sta = esp_netif_create_default_wifi_sta();
    assert(s_netif_sta);

    /* WiFi driver init — don't start radio yet, wifi_start() will do it */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers once */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    /* Set mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi subsystem initialized (radio OFF — call wifi_start())");
    return ESP_OK;
}

esp_err_t wifi_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    if (s_radio_on) {
        esp_wifi_stop();
        s_radio_on = false;
    }
    esp_wifi_deinit();
    /* netif and event loop cleanup is handled by ESP-IDF on deinit */

    s_initialized = false;
    s_connected   = false;
    clear_cached_info();
    ESP_LOGI(TAG, "WiFi deinitialized");
    return ESP_OK;
}

/* ========================================================================
 * 2. Radio On / Off — power control
 * ======================================================================== */

esp_err_t wifi_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_radio_on) {
        ESP_LOGW(TAG, "Radio already on");
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_radio_on = true;
    ESP_LOGI(TAG, "WiFi radio ON");
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    if (!s_initialized || !s_radio_on) return ESP_OK;

    /* If connected, disconnect first to trigger event cleanup */
    if (s_connected) {
        esp_wifi_disconnect();
        s_connected = false;
    }

    /* Wait for disconnect to propagate */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_ERROR_CHECK(esp_wifi_stop());
    s_radio_on = false;
    clear_cached_info();
    ESP_LOGI(TAG, "WiFi radio OFF");
    return ESP_OK;
}

/* ========================================================================
 * 3. Connect / Disconnect / Reconnect
 * ======================================================================== */

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || !password) {
        ESP_LOGE(TAG, "SSID and password must not be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Ensure radio is on */
    if (!s_radio_on) {
        esp_err_t ret = wifi_start();
        if (ret != ESP_OK) return ret;
    }

    /* Disconnect if already connected */
    if (s_connected) {
        wifi_disconnect();
    }

    /* Save credentials for reconnect */
    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
    strncpy(s_saved_password, password, sizeof(s_saved_password) - 1);

    esp_err_t ret = connect_internal(ssid, password);
    if (ret != ESP_OK) return ret;

    return wait_for_connection();
}

esp_err_t wifi_reconnect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_saved_ssid[0] == '\0') {
        ESP_LOGE(TAG, "No saved credentials — call wifi_connect() or wifi_set_config() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Reconnecting to \"%s\"...", s_saved_ssid);

    if (!s_radio_on) {
        esp_err_t ret = wifi_start();
        if (ret != ESP_OK) return ret;
    }
    if (s_connected) {
        wifi_disconnect();
    }

    esp_err_t ret = connect_internal(s_saved_ssid, s_saved_password);
    if (ret != ESP_OK) return ret;

    return wait_for_connection();
}

esp_err_t wifi_disconnect(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!s_connected)   return ESP_OK;

    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        s_connected = false;
        clear_cached_info();
        ESP_LOGI(TAG, "Disconnected");
    }
    return ret;
}

/* ========================================================================
 * 4. Scan
 * ======================================================================== */

esp_err_t wifi_scan(wifi_ap_record_t *records, uint16_t max_count,
                    uint16_t *found_count, uint32_t timeout_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!records || !found_count || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Ensure radio is on (scan needs it) */
    if (!s_radio_on) {
        esp_err_t ret = wifi_start();
        if (ret != ESP_OK) return ret;
    }

    /* Temporarily disconnect for full-channel scan */
    bool was_connected = s_connected;
    wifi_config_t saved_cfg = { 0 };
    if (was_connected) {
        esp_wifi_get_config(WIFI_IF_STA, &saved_cfg);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xEventGroupClearBits(s_event_group, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    ESP_LOGI(TAG, "Scanning...");
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        *found_count = 0;
        goto reconnect;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, WIFI_SCAN_DONE_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : WIFI_SCAN_TIMEOUT_MS));

    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        ESP_LOGW(TAG, "Scan timed out");
        *found_count = 0;
        ret = ESP_ERR_TIMEOUT;
        goto reconnect;
    }

    uint16_t total = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&total));
    if (total == 0) {
        ESP_LOGI(TAG, "No APs found");
        *found_count = 0;
        ret = ESP_OK;
        goto reconnect;
    }

    uint16_t count = (total < max_count) ? total : max_count;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&count, records));
    *found_count = count;

    ESP_LOGI(TAG, "Scan done: %d APs (showing %d)", total, count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  [%d] \"%s\"  RSSI=%d  CH=%d  %s",
                 i, records[i].ssid, records[i].rssi,
                 records[i].primary,
                 records[i].authmode == WIFI_AUTH_OPEN ? "(open)" : "");
    }
    ret = ESP_OK;

reconnect:
    if (was_connected && saved_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Reconnecting to \"%s\"...", saved_cfg.sta.ssid);
        connect_internal((const char *)saved_cfg.sta.ssid,
                         (const char *)saved_cfg.sta.password);
        wait_for_connection();
    }
    return ret;
}

/* ========================================================================
 * 5. Status / Info
 * ======================================================================== */

esp_err_t wifi_get_info(wifi_info_t *info)
{
    if (!s_initialized || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(info, &s_cached_info, sizeof(wifi_info_t));
    info->is_connected = s_connected;

    /* Refresh live values */
    if (s_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            info->rssi    = ap_info.rssi;
            info->channel = ap_info.primary;
        }
    }

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_initialized && s_connected;
}

/* ========================================================================
 * 6. Credential Management
 * ======================================================================== */

esp_err_t wifi_get_config(char *ssid, size_t ssid_len,
                          char *password, size_t pwd_len)
{
    if (!s_initialized || !ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(ssid, s_saved_ssid, ssid_len - 1);
    ssid[ssid_len - 1] = '\0';
    strncpy(password, s_saved_password, pwd_len - 1);
    password[pwd_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t wifi_set_config(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
    strncpy(s_saved_password, password, sizeof(s_saved_password) - 1);
    ESP_LOGI(TAG, "Credentials saved for \"%s\" (call wifi_reconnect() to use)", ssid);
    return ESP_OK;
}

/* ========================================================================
 * 7. Power Management
 * ======================================================================== */

esp_err_t wifi_set_power_save(wifi_ps_type_t mode)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    const char *names[] = { "NONE", "MIN_MODEM", "MAX_MODEM" };
    ESP_LOGI(TAG, "Power save: %s", names[mode]);
    return esp_wifi_set_ps(mode);
}

/* ========================================================================
 * 8. Event Callbacks
 * ======================================================================== */

void wifi_set_callbacks(wifi_connected_cb_t on_connected,
                        wifi_disconnected_cb_t on_disconnected)
{
    s_on_connected    = on_connected;
    s_on_disconnected = on_disconnected;
}

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

static esp_err_t connect_internal(const char *ssid, const char *password)
{
    xEventGroupClearBits(s_event_group,
                         WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t wait_for_connection(void)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group,
        WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
        pdTRUE,   /* clear on exit */
        pdFALSE,  /* wait for ANY bit */
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected — IP: %s", s_cached_info.ip);
        return ESP_OK;
    }
    if (bits & WIFI_DISCONNECTED_BIT) {
        ESP_LOGE(TAG, "Auth failed (wrong password?)");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGE(TAG, "Connection timed out (%d ms)", WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

static void update_cached_info(void)
{
    memset(&s_cached_info, 0, sizeof(s_cached_info));

    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        strncpy(s_cached_info.ssid, (const char *)cfg.sta.ssid,
                sizeof(s_cached_info.ssid) - 1);
    }

    esp_netif_ip_info_t ip_info;
    if (s_netif_sta && esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK) {
        snprintf(s_cached_info.ip, sizeof(s_cached_info.ip),
                 IPSTR, IP2STR(&ip_info.ip));
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_cached_info.rssi    = ap_info.rssi;
        s_cached_info.channel = ap_info.primary;
        snprintf(s_cached_info.bssid, sizeof(s_cached_info.bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
        s_cached_info.auth_mode = ap_info.authmode;
    }
}

static void clear_cached_info(void)
{
    memset(&s_cached_info, 0, sizeof(s_cached_info));
}

/* ========================================================================
 * Event Handler
 * ======================================================================== */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {

        switch (event_id) {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;

        case WIFI_EVENT_STA_STOP:
            ESP_LOGI(TAG, "STA stopped");
            xEventGroupSetBits(s_event_group, WIFI_STOPPED_BIT);
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Associated with AP (waiting for DHCP)...");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected (reason=%d)", ev->reason);
            s_connected = false;
            clear_cached_info();
            xEventGroupSetBits(s_event_group, WIFI_DISCONNECTED_BIT);
            if (s_on_disconnected) {
                s_on_disconnected();
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "Scan done");
            xEventGroupSetBits(s_event_group, WIFI_SCAN_DONE_BIT);
            break;

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        update_cached_info();
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        if (s_on_connected) {
            s_on_connected(&s_cached_info);
        }
    }
}
