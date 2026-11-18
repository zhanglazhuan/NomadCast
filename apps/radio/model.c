#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "model.h"
#include "app.h"
#include "flash_store.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "radio_model";

#define NS "radio"   /* NVS namespace (≤15 chars) */

/* cJSON allocates from PSRAM to keep scarce internal DRAM free (podcast idiom).
 * Hooks are process-wide; set here before any parse/serialize. */
static void *radio_psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}

/* ── station <-> JSON (favorites are one JSON blob per NVS key) ─────────── */

static char *station_to_json(const radio_station_t *s) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "u", s->uuid);
    cJSON_AddStringToObject(o, "n", s->name);
    cJSON_AddStringToObject(o, "r", s->region);
    cJSON_AddStringToObject(o, "rn", s->region_name);
    cJSON_AddStringToObject(o, "c", s->codec);
    cJSON_AddStringToObject(o, "f", s->favicon);
    cJSON_AddNumberToObject(o, "v", s->votes);
    cJSON_AddNumberToObject(o, "b", s->bitrate);
    cJSON_AddBoolToObject(o, "h", s->hls);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

static void copy_field(char *dst, size_t n, const cJSON *o, const char *key) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    const char *v = (it && cJSON_IsString(it)) ? it->valuestring : "";
    strncpy(dst, v, n - 1);
    dst[n - 1] = '\0';
}

static bool station_from_json(radio_station_t *s, const char *json) {
    if (!s || !json || !json[0]) return false;
    cJSON *o = cJSON_Parse(json);
    if (!o) return false;
    copy_field(s->uuid, sizeof(s->uuid), o, "u");
    copy_field(s->name, sizeof(s->name), o, "n");
    copy_field(s->region, sizeof(s->region), o, "r");
    copy_field(s->region_name, sizeof(s->region_name), o, "rn");
    copy_field(s->codec, sizeof(s->codec), o, "c");
    copy_field(s->favicon, sizeof(s->favicon), o, "f");
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, "v");
    s->votes = (it && cJSON_IsNumber(it)) ? it->valueint : 0;
    it = cJSON_GetObjectItemCaseSensitive(o, "b");
    s->bitrate = (it && cJSON_IsNumber(it)) ? it->valueint : 0;
    it = cJSON_GetObjectItemCaseSensitive(o, "h");
    s->hls = (it && cJSON_IsTrue(it));
    cJSON_Delete(o);
    return s->uuid[0] != '\0';
}

/* ── favorites persistence ──────────────────────────────────────────────── */

void radio_model_load_favorites(struct RadioApp* app) {
    RadioModel *m = app->model;
    if (!m) return;

    m->favorite_count = 0;
    int count = flash_get_i32(NS, "fav_count", 0);
    if (count < 0) count = 0;
    if (count > RADIO_MAX_FAVORITES) count = RADIO_MAX_FAVORITES;

    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "f%d", i);
        char json[512];
        if (flash_get_str(NS, key, json, sizeof(json), "") <= 0) continue;
        if (m->favorite_count >= RADIO_MAX_FAVORITES) break;
        if (station_from_json(&m->favorites[m->favorite_count], json))
            m->favorite_count++;
    }
    ESP_LOGI(TAG, "loaded %d favorites", m->favorite_count);
}

void radio_model_save_favorites(struct RadioApp* app) {
    RadioModel *m = app->model;
    if (!m) return;

    /* Erase stale slots first so shrinking the list doesn't leave ghosts. */
    for (int i = 0; i < RADIO_MAX_FAVORITES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "f%d", i);
        flash_set_str(NS, key, "");   /* overwrite with empty (NVS has no delete) */
    }

    for (int i = 0; i < m->favorite_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "f%d", i);
        char *json = station_to_json(&m->favorites[i]);
        if (!json) continue;
        flash_set_str(NS, key, json);
        free(json);
    }
    flash_set_i32(NS, "fav_count", m->favorite_count);
    ESP_LOGI(TAG, "saved %d favorites", m->favorite_count);
}

bool radio_model_is_favorite(struct RadioApp* app, const char *uuid) {
    RadioModel *m = app->model;
    if (!m || !uuid || !uuid[0]) return false;
    for (int i = 0; i < m->favorite_count; i++) {
        if (strcmp(m->favorites[i].uuid, uuid) == 0) return true;
    }
    return false;
}

bool radio_model_toggle_favorite(struct RadioApp* app, const radio_station_t *s) {
    RadioModel *m = app->model;
    if (!m || !s) return false;

    /* Remove if present. */
    for (int i = 0; i < m->favorite_count; i++) {
        if (strcmp(m->favorites[i].uuid, s->uuid) == 0) {
            memmove(&m->favorites[i], &m->favorites[i + 1],
                    (m->favorite_count - i - 1) * sizeof(radio_station_t));
            m->favorite_count--;
            radio_model_save_favorites(app);
            return false;   /* now unfavorited */
        }
    }

    /* Add at the front (newest first). */
    if (m->favorite_count >= RADIO_MAX_FAVORITES)
        m->favorite_count = RADIO_MAX_FAVORITES - 1;
    memmove(&m->favorites[1], &m->favorites[0],
            m->favorite_count * sizeof(radio_station_t));
    m->favorites[0] = *s;
    m->favorite_count++;
    radio_model_save_favorites(app);
    return true;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

void radio_model_init(struct RadioApp* app) {
    app->model = (RadioModel*)calloc(1, sizeof(RadioModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model alloc failed");
        return;
    }

    cJSON_Hooks hooks = { .malloc_fn = radio_psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    /* Radio country follows the device's 所在国家/地区 (system region), stored by
     * Settings under the "podcast" NVS namespace as a lowercase code ("cn"/"us"/…):
     * mainland China → CN, every other region → US. Not user-switchable in radio. */
    char cc[8] = {0};
    flash_get_str("podcast", "country", cc, sizeof(cc), "cn");
    strcpy(app->model->country, (strcmp(cc, "cn") == 0) ? "CN" : "US");

    app->model->current = -1;
    app->model->play_source = RADIO_SRC_STATIONS;
    app->model->current_page = 0;

    radio_model_load_favorites(app);
    ESP_LOGI(TAG, "init done");
}

void radio_model_deinit(struct RadioApp* app) {
    if (app->model) {
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
