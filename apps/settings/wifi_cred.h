/**
 * @file wifi_cred.h
 * @brief WiFi credential storage — NVS-backed, most-recent-first ordering.
 *
 * Stores up to 10 SSID/password pairs in the "wifi_cred" NVS namespace.
 * On save, existing entries for the same SSID are deduplicated and the
 * entry is moved to the end (most recent).  Load returns entries newest-first.
 */

#ifndef WIFI_CRED_H
#define WIFI_CRED_H

#include <stdbool.h>

#define MAX_WIFI_CREDS  10
#define WIFI_CRED_NS    "wifi_cred"

typedef struct {
    char ssid[32];
    char password[64];
} wifi_cred_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Save (or update) a credential — moves to most-recent position.
 */
void wifi_cred_save(const char *ssid, const char *password);

/**
 * @brief Load all credentials, newest first.  Returns count (0..MAX_WIFI_CREDS).
 *        Skips duplicate SSIDs (keeps only the newest entry).
 */
int wifi_cred_load_all(wifi_cred_t *out, int max);

/**
 * @brief Find password for a given SSID.
 * @return true if found, false otherwise.
 */
bool wifi_cred_find(const char *ssid, char *password_out, int password_size);

/**
 * @brief Delete a specific credential by SSID.
 */
void wifi_cred_delete(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CRED_H */
