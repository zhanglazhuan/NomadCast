/*
 * t_wifi — WiFi Scan + Connect + HTTP Download to SD Card
 *
 *   1. Scan and print all nearby WiFi networks
 *   2. Connect to configured AP
 *   3. Mount SD card (FATFS, 1-bit SDMMC)
 *   4. HTTP GET a file to /sdcard/downloaded_file
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

static const char *TAG = "t_wifi";

#define WIFI_SSID       "zhanglazhuan"
#define WIFI_PASSWORD   "zhangla1991"
#define WIFI_TIMEOUT_MS 15000
#define SCAN_MAX_AP     30

#define DOWNLOAD_URL    "http://fuss10.elemecdn.com/e/5d/4a731a90594a4af544c0c25941171jpeg.jpeg"
#define SAVE_PATH       "/sdcard/downloaded_file"

#define PIN_SD_CLK      GPIO_NUM_1
#define PIN_SD_CMD      GPIO_NUM_14
#define PIN_SD_D0       GPIO_NUM_2
#define SD_MOUNT_POINT  "/sdcard"

/* ========================================================================
 * WiFi Scan
 * ======================================================================== */

static void wifi_scan(void)
{
    ESP_LOGI(TAG, "=== WiFi Scan ===");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);  /* blocking */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);

    wifi_ap_record_t aps[SCAN_MAX_AP];
    if (count > SCAN_MAX_AP) count = SCAN_MAX_AP;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&count, aps));

    ESP_LOGI(TAG, "Found %d APs:", count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  [%2d] %-32s  CH=%2d  RSSI=%4d  %s",
                 i, aps[i].ssid, aps[i].primary, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? "(open)" : "");
    }
    ESP_LOGI(TAG, "=== Scan Done ===");
}

/* ========================================================================
 * WiFi Connect
 * ======================================================================== */

static EventGroupHandle_t wifi_evt;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    /* Do NOT auto-connect on STA_START — we scan first */
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

static esp_err_t wifi_connect_sta(const char *ssid, const char *pwd)
{
    wifi_evt = xEventGroupCreate();

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pwd, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to %s...", ssid);
    EventBits_t bits = xEventGroupWaitBits(wifi_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

/* ========================================================================
 * SD Card
 * ======================================================================== */

static esp_err_t sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK; slot.cmd = PIN_SD_CMD; slot.d0 = PIN_SD_D0;
    slot.width = 1;
    esp_vfs_fat_sdmmc_mount_config_t mnt = { .format_if_mount_failed = false, .max_files = 5 };
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &card);
}

/* ========================================================================
 * HTTP Download
 * ======================================================================== */

static esp_err_t http_download(const char *url, const char *path)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 30000, .buffer_size = 4096 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) goto cleanup;

    int len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, Content-Length: %d", status, len);
    if (status != 200) { err = ESP_FAIL; goto close; }

    FILE *fp = fopen(path, "wb");
    if (!fp) { err = ESP_FAIL; goto close; }

    char buf[4096]; int total = 0, n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, fp); total += n;
        ESP_LOGI(TAG, "  %d / %d bytes", total, len);
    }
    fclose(fp);
    ESP_LOGI(TAG, "Done: %s (%d bytes)", path, total);

close:
    esp_http_client_close(client);
cleanup:
    esp_http_client_cleanup(client);
    return err;
}

/* ======================================================================== */

/* Scan + connect + download — needs large stack, runs in own task */
static void wifi_worker_task(void *arg)
{
    /* [3] Scan */
    ESP_LOGI(TAG, "[3] Scan...");
    wifi_scan();

    /* [4] Connect */
    ESP_LOGI(TAG, "[4] Connect...");
    esp_err_t ret = wifi_connect_sta(WIFI_SSID, WIFI_PASSWORD);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "WiFi fail"); vTaskDelete(NULL); return; }

    /* [5] HTTP Download */
    ESP_LOGI(TAG, "[5] HTTP download test...");
    esp_http_client_config_t cfg = {
        .url = DOWNLOAD_URL,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ret = esp_http_client_open(client, 0);
    if (ret == ESP_OK) {
        int len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d, Content-Length: %d", status, len);
        if (status == 200) {
            char buf[4096]; int total = 0, n;
            while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) total += n;
            ESP_LOGI(TAG, "Downloaded: %d bytes (%.1f KB)", total, total / 1024.0f);
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "=== Done ===");
    while (1) vTaskDelay(5000);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_wifi ===");

    /* [1] NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) { nvs_flash_erase(); nvs_flash_init(); }

    /* [2] WiFi init (MUST run in main task context) */
    ESP_LOGI(TAG, "[2] WiFi init...");
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

    /* Worker task: scan → connect → download (needs large stack) */
    xTaskCreate(wifi_worker_task, "wifi_work", 8192, NULL, 5, NULL);
    while (1) vTaskDelay(5000);
}
