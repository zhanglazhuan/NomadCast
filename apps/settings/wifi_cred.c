/**
 * @file wifi_cred.c
 * @brief WiFi credential storage — numbered NVS slots, newest-last append.
 */

#include <stdio.h>
#include <string.h>
#include "wifi_cred.h"
#include "flash_store.h"
#include "esp_log.h"

static const char *TAG = "wifi_cred";

/* Slot key helpers: "s0", "p0", "s1", "p1", ... */
static void slot_key(char *buf, int buflen, char prefix, int idx)
{
    snprintf(buf, buflen, "%c%d", prefix, idx);
}

void wifi_cred_save(const char *ssid, const char *password)
{
    int count = flash_get_i32(WIFI_CRED_NS, "count", 0);

    /* First pass: check if this SSID already exists — if so, overwrite in place
     * (no need to move — load_all deduplicates by keeping only the newest) */
    for (int i = 0; i < count && i < 99; i++) {
        char ks[8], saved[32];
        slot_key(ks, sizeof(ks), 's', i);
        if (flash_get_str(WIFI_CRED_NS, ks, saved, sizeof(saved), NULL) > 0 &&
            strcmp(saved, ssid) == 0) {
            /* Update password in-place */
            char kp[8];
            slot_key(kp, sizeof(kp), 'p', i);
            flash_set_str(WIFI_CRED_NS, kp, password);
            ESP_LOGI(TAG, "Updated credential for '%s' at slot %d", ssid, i);
            return;
        }
    }

    /* Not found — append at 'count' position */
    if (count >= MAX_WIFI_CREDS) {
        /* Shift all slots down by 1 (drop oldest at index 0) */
        for (int i = 0; i < count - 1; i++) {
            char ks_src[8], ks_dst[8], kp_src[8], kp_dst[8];
            char buf_ssid[32], buf_pwd[64];

            slot_key(ks_src, sizeof(ks_src), 's', i + 1);
            slot_key(ks_dst, sizeof(ks_dst), 's', i);
            slot_key(kp_src, sizeof(kp_src), 'p', i + 1);
            slot_key(kp_dst, sizeof(kp_dst), 'p', i);

            if (flash_get_str(WIFI_CRED_NS, ks_src, buf_ssid, sizeof(buf_ssid), NULL) > 0) {
                flash_set_str(WIFI_CRED_NS, ks_dst, buf_ssid);
            }
            if (flash_get_str(WIFI_CRED_NS, kp_src, buf_pwd, sizeof(buf_pwd), NULL) > 0) {
                flash_set_str(WIFI_CRED_NS, kp_dst, buf_pwd);
            }
        }
        count = MAX_WIFI_CREDS - 1;
    }

    char ks[8], kp[8];
    slot_key(ks, sizeof(ks), 's', count);
    slot_key(kp, sizeof(kp), 'p', count);
    flash_set_str(WIFI_CRED_NS, ks, ssid);
    flash_set_str(WIFI_CRED_NS, kp, password);
    flash_set_i32(WIFI_CRED_NS, "count", count + 1);

    ESP_LOGI(TAG, "Saved credential for '%s' at slot %d (total %d)", ssid, count, count + 1);
}

int wifi_cred_load_all(wifi_cred_t *out, int max)
{
    int count = flash_get_i32(WIFI_CRED_NS, "count", 0);
    int out_count = 0;

    /* Scan from newest (highest index) to oldest (index 0).
     * Skip duplicate SSIDs — keep only the first occurrence from the end. */
    for (int i = count - 1; i >= 0 && out_count < max; i--) {
        char ks[8], kp[8];
        char ssid[32], pwd[64];

        slot_key(ks, sizeof(ks), 's', i);
        slot_key(kp, sizeof(kp), 'p', i);

        if (flash_get_str(WIFI_CRED_NS, ks, ssid, sizeof(ssid), NULL) <= 0 || ssid[0] == '\0')
            continue;

        /* Skip if we already have this SSID (keep the newer one) */
        bool dup = false;
        for (int j = 0; j < out_count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;

        flash_get_str(WIFI_CRED_NS, kp, pwd, sizeof(pwd), "");

        strncpy(out[out_count].ssid, ssid, sizeof(out[out_count].ssid) - 1);
        out[out_count].ssid[sizeof(out[out_count].ssid) - 1] = '\0';
        strncpy(out[out_count].password, pwd, sizeof(out[out_count].password) - 1);
        out[out_count].password[sizeof(out[out_count].password) - 1] = '\0';
        out_count++;
    }

    ESP_LOGI(TAG, "Loaded %d credentials (from %d slots)", out_count, count);
    return out_count;
}

bool wifi_cred_find(const char *ssid, char *password_out, int password_size)
{
    int count = flash_get_i32(WIFI_CRED_NS, "count", 0);

    /* Scan newest first */
    for (int i = count - 1; i >= 0; i--) {
        char ks[8], saved[32];
        slot_key(ks, sizeof(ks), 's', i);

        if (flash_get_str(WIFI_CRED_NS, ks, saved, sizeof(saved), NULL) > 0 &&
            strcmp(saved, ssid) == 0) {
            char kp[8];
            slot_key(kp, sizeof(kp), 'p', i);
            flash_get_str(WIFI_CRED_NS, kp, password_out, password_size, "");
            ESP_LOGI(TAG, "Found credential for '%s'", ssid);
            return true;
        }
    }
    return false;
}

void wifi_cred_delete(const char *ssid)
{
    int count = flash_get_i32(WIFI_CRED_NS, "count", 0);

    for (int i = 0; i < count; i++) {
        char ks[8], saved[32];
        slot_key(ks, sizeof(ks), 's', i);

        if (flash_get_str(WIFI_CRED_NS, ks, saved, sizeof(saved), NULL) > 0 &&
            strcmp(saved, ssid) == 0) {
            /* Clear this slot */
            flash_set_str(WIFI_CRED_NS, ks, "");
            char kp[8];
            slot_key(kp, sizeof(kp), 'p', i);
            flash_set_str(WIFI_CRED_NS, kp, "");
            ESP_LOGI(TAG, "Deleted credential for '%s'", ssid);
            return;
        }
    }
}
