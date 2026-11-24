/*
 * NomadCast — Flash Store (NVS-backed key-value persistence)
 */

#include "flash_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "flash";
static const char *USER_PARTITION = "nvs_user";
static bool s_initialized = false;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    esp_err_t err = nvs_open_from_partition(USER_PARTITION, ns, mode, h);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS not init — re-initializing");
        flash_store_init();
        err = nvs_open_from_partition(USER_PARTITION, ns, mode, h);
    }
    return err;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void flash_store_init(void)
{
    if (s_initialized) return;

    /* The default nvs partition is reserved for factory/system data.
     * Application settings live exclusively in the separately erasable user
     * partition. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    err = nvs_flash_init_partition(USER_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "User NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase_partition(USER_PARTITION));
        err = nvs_flash_init_partition(USER_PARTITION);
    }
    ESP_ERROR_CHECK(err);
    s_initialized = true;
    ESP_LOGI(TAG, "Ready");
}

/* ── Int32 ────────────────────────────────────────────────────────────────── */

int32_t flash_get_i32(const char *ns, const char *key, int32_t def)
{
    nvs_handle_t h;
    if (open_ns(ns, NVS_READONLY, &h) != ESP_OK) return def;

    int32_t val = def;
    esp_err_t err = nvs_get_i32(h, key, &val);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Write default so next boot finds it */
        flash_set_i32(ns, key, def);
    }
    return val;
}

void flash_set_i32(const char *ns, const char *key, int32_t val)
{
    nvs_handle_t h;
    if (open_ns(ns, NVS_READWRITE, &h) != ESP_OK) return;

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_i32(h, key, val));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(h));
    nvs_close(h);
}

/* ── Bool ─────────────────────────────────────────────────────────────────── */

bool flash_get_bool(const char *ns, const char *key, bool def)
{
    return flash_get_i32(ns, key, def ? 1 : 0) != 0;
}

void flash_set_bool(const char *ns, const char *key, bool val)
{
    flash_set_i32(ns, key, val ? 1 : 0);
}

/* ── String ───────────────────────────────────────────────────────────────── */

int flash_get_str(const char *ns, const char *key, char *out, size_t max, const char *def)
{
    nvs_handle_t h;
    if (open_ns(ns, NVS_READONLY, &h) != ESP_OK) {
        if (def) { strncpy(out, def, max - 1); out[max - 1] = '\0'; }
        return def ? (int)strlen(def) : 0;
    }

    size_t len = max;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Write default so next boot finds it */
        if (def) {
            strncpy(out, def, max - 1); out[max - 1] = '\0';
            flash_set_str(ns, key, def);
            return (int)strlen(def);
        }
        out[0] = '\0';
        return 0;
    }

    return (int)len;
}

void flash_set_str(const char *ns, const char *key, const char *val)
{
    if (!val) return;
    nvs_handle_t h;
    if (open_ns(ns, NVS_READWRITE, &h) != ESP_OK) return;

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(h, key, val));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(h));
    nvs_close(h);
}

/* ── Device ID ────────────────────────────────────────────────────────────── */

int flash_get_device_id(char *out, size_t max)
{
    if (!out || max == 0) return 0;

    int len = flash_get_str("podcast", "devid", out, max, "");
    if (len > 0 && out[0]) return len;

    /* No persisted ID yet — derive it from the WiFi STA MAC and store it. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, max, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    flash_set_str("podcast", "devid", out);
    ESP_LOGI(TAG, "Generated device ID: %s", out);
    return (int)strlen(out);
}

/* ── Erase ────────────────────────────────────────────────────────────────── */

void flash_erase_ns(const char *ns)
{
    nvs_handle_t h;
    if (open_ns(ns, NVS_READWRITE, &h) != ESP_OK) return;

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_erase_all(h));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Erased namespace '%s'", ns);
}

void flash_erase_all(void)
{
    ESP_LOGI(TAG, "Erasing user NVS partition...");
    ESP_ERROR_CHECK(nvs_flash_erase_partition(USER_PARTITION));
    ESP_LOGI(TAG, "User NVS erased. System NVS preserved.");
}
