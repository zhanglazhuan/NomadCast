/**
 * t_podcast — direct server chart fetch + cJSON parse test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "t_podcast";

#define WIFI_SSID       "zhanglazhuan"
#define WIFI_PASSWORD   "zhangla1991"
#define WIFI_TIMEOUT_MS 15000
#define SERVER_URL      "http://192.168.137.1:5000"

static EventGroupHandle_t s_wifi_evt;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
}

static bool wifi_connect(void) {
    s_wifi_evt = xEventGroupCreate();
    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static char *http_get(const char *url, int *out_len) {
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client); return NULL;
    }
    int clen = esp_http_client_fetch_headers(client);
    int st = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, Content-Length=%d", st, clen);
    if (st != 200) { esp_http_client_cleanup(client); return NULL; }

    int cap = (clen > 0) ? clen + 1 : 16384;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) { esp_http_client_cleanup(client); return NULL; }
    int total = 0;
    while (1) {
        int n = esp_http_client_read(client, body + total, cap - total - 1);
        if (n <= 0) break;
        total += n;
        if (cap - total < 1024) { cap *= 2; body = heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM); }
    }
    body[total] = '\0';
    *out_len = total;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Downloaded %d bytes", total);
    return body;
}

void app_main(void) {
    ESP_LOGI(TAG, "=== t_podcast server test ===");
    nvs_flash_init();
    esp_netif_init(); esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
    if (!wifi_connect()) { ESP_LOGE(TAG, "WiFi fail"); while(1) vTaskDelay(5000); }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Fetch chart */
    char url[1024];
    snprintf(url, sizeof(url), "%s/api/charts/full?country=cn&limit=3", SERVER_URL);
    int len;
    char *body = http_get(url, &len);
    if (!body) { ESP_LOGE(TAG, "HTTP fail"); while(1) vTaskDelay(5000); }

    /* Dump first 100 and last 20 bytes */
    ESP_LOGI(TAG, "Body: %d bytes, first 100: %.100s", len, body);
    ESP_LOGI(TAG, "Last 20: %.20s", body + len - 20);

    /* Parse with cJSON */
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        int pos = cJSON_GetErrorPtr() ? (int)(cJSON_GetErrorPtr() - body) : -1;
        ESP_LOGE(TAG, "cJSON fail at %d, char=0x%02X", pos, (unsigned char)body[pos]);
    } else {
        cJSON *feed = cJSON_GetObjectItem(root, "feed");
        cJSON *results = feed ? cJSON_GetObjectItem(feed, "results") : NULL;
        int count = results ? cJSON_GetArraySize(results) : 0;
        ESP_LOGI(TAG, "Parsed OK: %d channels", count);
        for (int i = 0; i < count && i < 3; i++) {
            cJSON *item = cJSON_GetArrayItem(results, i);
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *feedUrl = cJSON_GetObjectItem(item, "feedUrl");
            cJSON *tc = cJSON_GetObjectItem(item, "trackCount");
            ESP_LOGI(TAG, "  [%d] %s | feed=%s | episodes=%d",
                     i,
                     name ? name->valuestring : "?",
                     feedUrl ? feedUrl->valuestring : "?",
                     tc ? tc->valueint : 0);
        }
        cJSON_Delete(root);
    }
    free(body);
    ESP_LOGI(TAG, "=== Done ===");
    while(1) vTaskDelay(5000);
}
