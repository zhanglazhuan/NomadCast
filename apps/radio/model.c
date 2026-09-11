#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "app.h"
#include "preset_stations.h"
#include "lang.h"
#include "esp_log.h"

static const char *TAG = "radio_model";

void radio_model_init(struct RadioApp* app) {
    app->model = (RadioModel*)calloc(1, sizeof(RadioModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model alloc failed");
        return;
    }

    /* Select preset table by UI language: Chinese → domestic (CNR + provincial)
     * stations, English → American (SomaFM) stations. Re-selected on every app
     * open, so a runtime language switch is picked up on the next launch. */
    if (lang_get() == LANG_ZH_CN) {
        app->model->stations = g_stations_cn;
        app->model->count    = g_stations_cn_count;
    } else {
        app->model->stations = g_stations_en;
        app->model->count    = g_stations_en_count;
    }
    app->model->current  = -1;
    app->model->current_page = 0;
    ESP_LOGI(TAG, "init done (%d stations, lang=%d)", app->model->count, (int)lang_get());
}

void radio_model_deinit(struct RadioApp* app) {
    if (app->model) {
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
