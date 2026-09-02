/*
 * t_ota — OTA firmware-update test (reuses system/ota)
 *
 *   1. Connect WiFi
 *   2. ota_init()
 *   3. ota_check(manifest_url) — download manifest.json, compare version
 *   4. If update available → ota_perform(firmware_url) → esp_restart()
 *
 * Test procedure:
 *   - Flash this firmware at version 0.1.0 (factory) — idf.py flash.
 *   - Rebuild at version 0.1.1 (bump version.txt), drop its .bin into
 *     server/firmware/ and set server/firmware/version.txt to "0.1.1".
 *     The Flask server (server/server.py) serves it via /api/ota/download.
 *   - Device boots → connects WiFi → sees 0.1.1 > 0.1.0 → downloads + flashes
 *     → reboots → serial log shows "Running: t_ota v0.1.1".
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "ota.h"

static const char *TAG = "t_ota";

#define WIFI_SSID       "zhanglazhuan"
#define WIFI_PASSWORD   "zhangla1991"
#define WIFI_TIMEOUT_MS 15000

/* Manifest URL — the Flask server's /api/ota/check endpoint (server/server.py).
 * It returns {"version":…,"url":"http://<host>/api/ota/download",…}. */
#define MANIFEST_URL    "http://192.168.137.1:5000/api/ota/check"

/* ── WiFi connect ──────────────────────────────────────────────────────── */

static EventGroupHandle_t wifi_evt;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi disconnected: reason=%d", d->reason);
        xEventGroupSetBits(wifi_evt, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void)
{
    wifi_evt = xEventGroupCreate();

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(wifi_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* ── OTA callbacks ─────────────────────────────────────────────────────── */

static void on_progress(int percent, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "OTA progress: %d%%", percent);
}

static void on_checked(ota_check_result_t result,
                       const char *new_version,
                       const char *changelog,
                       const char *firmware_url,
                       void *user_data)
{
    (void)user_data;

    switch (result) {
    case OTA_CHECK_UP_TO_DATE:
        ESP_LOGI(TAG, "Already up-to-date (no update)");
        break;

    case OTA_CHECK_UPDATE_AVAILABLE:
        ESP_LOGI(TAG, "Update available: v%s (%s) from %s",
                 new_version, changelog, firmware_url);
        if (ota_perform(firmware_url, on_progress, NULL)) {
            ESP_LOGI(TAG, "OTA complete — rebooting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA failed");
        }
        break;

    case OTA_CHECK_ERROR_NETWORK:
        ESP_LOGE(TAG, "Check failed: network / server unreachable");
        break;

    case OTA_CHECK_ERROR_PARSE:
        ESP_LOGE(TAG, "Check failed: manifest parse error");
        break;

    default:
        ESP_LOGE(TAG, "Check failed: %d", result);
        break;
    }
}

/* ── Worker task: connect → check → update ────────────────────────────── */

static void ota_worker_task(void *arg)
{
    esp_err_t ret = wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        vTaskDelete(NULL);
        return;
    }

    ota_init();
    ota_check(MANIFEST_URL, on_checked, NULL);

    ESP_LOGI(TAG, "=== OTA test done ===");
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_ota ===");

    /* [1] NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) { nvs_flash_erase(); nvs_flash_init(); }

    /* [2] WiFi init (main task context) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* [3] Worker task: connect → check → download+flash → reboot */
    xTaskCreate(ota_worker_task, "ota_work", 8192, NULL, 5, NULL);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
