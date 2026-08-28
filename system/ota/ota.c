/*
 * NomadCast — WiFi OTA Firmware Update
 */

#include "ota.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_event.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static const char *TAG = "ota";

/* ── Semver compare ────────────────────────────────────────────────────────── */

/* Compare dotted "x.y.z" versions. Returns >0 if a is newer, <0 if older,
 * 0 if equal. Falls back to strcmp for non-numeric version strings. */
static int version_compare(const char *a, const char *b)
{
    int va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    if (sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]) < 1 ||
        sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]) < 1) {
        return strcmp(a, b);
    }
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

/* ── Progress tracking ────────────────────────────────────────────────────── */

typedef struct {
    ota_progress_cb_t progress_cb;
    void *progress_user_data;
    int content_length;
    int bytes_written;
    int last_pct;
} ota_state_t;

static ota_state_t s_ota;

static void on_ota_event(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != ESP_HTTPS_OTA_EVENT) return;

    switch (event_id) {
    case ESP_HTTPS_OTA_START:
        ESP_LOGI(TAG, "OTA started");
        s_ota.bytes_written = 0;
        s_ota.last_pct = -1;
        break;
    case ESP_HTTPS_OTA_CONNECTED:
        ESP_LOGI(TAG, "Connected to server");
        break;
    case ESP_HTTPS_OTA_GET_IMG_DESC:
        ESP_LOGI(TAG, "Reading image description...");
        break;
    case ESP_HTTPS_OTA_WRITE_FLASH:
        /* event_data points to the cumulative bytes written so far */
        s_ota.bytes_written = *(const int *)event_data;
        if (s_ota.content_length > 0 && s_ota.progress_cb) {
            int pct = (int)((s_ota.bytes_written * 100LL) / s_ota.content_length);
            if (pct > 100) pct = 100;
            if (pct < 0)   pct = 0;
            if (pct != s_ota.last_pct) {   /* only report on change — avoids log spam */
                s_ota.last_pct = pct;
                s_ota.progress_cb(pct, s_ota.progress_user_data);
            }
        }
        break;
    case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION:
        ESP_LOGI(TAG, "Boot partition updated");
        break;
    case ESP_HTTPS_OTA_FINISH:
        ESP_LOGI(TAG, "OTA finished");
        if (s_ota.progress_cb) s_ota.progress_cb(100, s_ota.progress_user_data);
        break;
    case ESP_HTTPS_OTA_ABORT:
        ESP_LOGE(TAG, "OTA aborted");
        break;
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void ota_init(void)
{
    /* Cancel rollback if bootloader rollback is enabled.
     * This confirms the current firmware is working. */
    esp_ota_mark_app_valid_cancel_rollback();

    /* Register OTA event handler for progress tracking */
    esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                               on_ota_event, NULL);

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "Running: %s v%s", desc->project_name, desc->version);
}

/* ── Manifest download helper ─────────────────────────────────────────────── */

typedef struct {
    ota_check_cb_t cb;
    void          *user_data;
    char           buffer[4096];
    int            len;
} manifest_ctx_t;

static esp_err_t manifest_http_handler(esp_http_client_event_t *evt)
{
    manifest_ctx_t *ctx = (manifest_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->len + evt->data_len < sizeof(ctx->buffer) - 1) {
            memcpy(ctx->buffer + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->buffer[ctx->len] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH: {
        ctx->buffer[ctx->len] = '\0';

        /* Parse JSON */
        cJSON *root = cJSON_Parse(ctx->buffer);
        if (!root) {
            ESP_LOGE(TAG, "manifest parse error");
            ctx->cb(OTA_CHECK_ERROR_PARSE, NULL, NULL, NULL, NULL, ctx->user_data);
            free(ctx);
            return ESP_OK;
        }

        cJSON *ver = cJSON_GetObjectItem(root, "version");
        cJSON *chg = cJSON_GetObjectItem(root, "changelog");
        cJSON *url = cJSON_GetObjectItem(root, "url");
        cJSON *dt  = cJSON_GetObjectItem(root, "date");

        if (!ver || !ver->valuestring) {
            cJSON_Delete(root);
            ctx->cb(OTA_CHECK_ERROR_PARSE, NULL, NULL, NULL, NULL, ctx->user_data);
            free(ctx);
            return ESP_OK;
        }

        const esp_app_desc_t *running = esp_app_get_description();
        const char *new_ver = ver->valuestring;
        const char *change  = chg ? chg->valuestring : "";
        const char *fw_url  = url ? url->valuestring : "";
        const char *release_date = dt ? dt->valuestring : "";

        ESP_LOGI(TAG, "running=%s remote=%s", running->version, new_ver);

        if (version_compare(new_ver, running->version) > 0) {
            ctx->cb(OTA_CHECK_UPDATE_AVAILABLE, new_ver, change, fw_url, release_date, ctx->user_data);
        } else {
            ctx->cb(OTA_CHECK_UP_TO_DATE, new_ver, change, fw_url, release_date, ctx->user_data);
        }

        cJSON_Delete(root);
        free(ctx);
        break;
    }
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "manifest HTTP error");
        ctx->cb(OTA_CHECK_ERROR_NETWORK, NULL, NULL, NULL, NULL, ctx->user_data);
        free(ctx);
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void ota_check(const char *manifest_url, ota_check_cb_t cb, void *user_data)
{
    if (!manifest_url || !cb) return;

    ESP_LOGI(TAG, "Checking update: %s", manifest_url);

    manifest_ctx_t *ctx = calloc(1, sizeof(manifest_ctx_t));
    if (!ctx) { cb(OTA_CHECK_ERROR_NETWORK, NULL, NULL, NULL, NULL, user_data); return; }
    ctx->cb = cb;
    ctx->user_data = user_data;

    esp_http_client_config_t http_cfg = {
        .url = manifest_url,
        .event_handler = manifest_http_handler,
        .user_data = ctx,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "client init failed — invalid URL?");
        ctx->cb(OTA_CHECK_ERROR_NETWORK, NULL, NULL, NULL, NULL, user_data);
        free(ctx);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest HTTP failed: %d", err);
        esp_http_client_cleanup(client);
        ctx->cb(OTA_CHECK_ERROR_NETWORK, NULL, NULL, NULL, NULL, user_data);
        free(ctx);
        return;
    }

    esp_http_client_cleanup(client);
    /* ctx is freed in the event handler */
}

bool ota_perform(const char *firmware_url, ota_progress_cb_t progress_cb, void *user_data)
{
    if (!firmware_url) return false;

    s_ota.progress_cb = progress_cb;
    s_ota.progress_user_data = user_data;
    s_ota.content_length = 0;

    ESP_LOGI(TAG, "Starting OTA from: %s", firmware_url);

    /* First do a HEAD or quick GET to determine content length */
    esp_http_client_config_t head_cfg = {
        .url = firmware_url,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t head = esp_http_client_init(&head_cfg);
    if (!head) {
        ESP_LOGE(TAG, "HEAD init failed — invalid URL?");
        s_ota.progress_cb = NULL;
        return false;
    }
    esp_http_client_set_method(head, HTTP_METHOD_HEAD);
    esp_err_t err = esp_http_client_perform(head);
    if (err == ESP_OK) {
        s_ota.content_length = esp_http_client_get_content_length(head);
        ESP_LOGI(TAG, "Firmware size: %d bytes", s_ota.content_length);
    }
    esp_http_client_cleanup(head);

    /* OTA download + flash */
    esp_https_ota_config_t ota_cfg = {
        .http_config = &(esp_http_client_config_t){
            .url = firmware_url,
            .timeout_ms = 120000,
            .buffer_size = 4096,
        },
        .bulk_flash_erase = true,
    };

    err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        s_ota.progress_cb = NULL;
        return false;
    }

    ESP_LOGI(TAG, "OTA complete — reboot to apply");
    s_ota.progress_cb = NULL;
    return true;
}

/* ── Auto update ──────────────────────────────────────────────────────────── */

static void auto_update_cb(ota_check_result_t result,
                           const char *new_version,
                           const char *changelog,
                           const char *firmware_url,
                           const char *release_date,
                           void *user_data)
{
    (void)changelog; (void)release_date; (void)user_data;

    if (result == OTA_CHECK_UPDATE_AVAILABLE && firmware_url && firmware_url[0]) {
        ESP_LOGI(TAG, "Auto-update: installing v%s", new_version);
        if (ota_perform(firmware_url, NULL, NULL)) {
            ESP_LOGI(TAG, "Auto-update complete — rebooting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Auto-update: install failed");
        }
    } else {
        ESP_LOGI(TAG, "Auto-update: no update (result %d)", (int)result);
    }
}

static void auto_update_task(void *arg)
{
    char *url = (char *)arg;
    ota_check(url, auto_update_cb, NULL);
    free(url);
    vTaskDelete(NULL);
}

void ota_auto_update_check(const char *manifest_url)
{
    if (!manifest_url || !manifest_url[0]) return;
    char *url = strdup(manifest_url);
    if (!url) return;
    ESP_LOGI(TAG, "Auto-update check: %s", manifest_url);
    xTaskCreate(auto_update_task, "ota_auto", 8192, url, 5, NULL);
}
