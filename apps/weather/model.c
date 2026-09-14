#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "model.h"
#include "app.h"
#include "flash_store.h"
#include "esp_log.h"

static const char *TAG = "weather_model";

#define NS "weather"   /* NVS namespace (≤15 chars) */

void weather_model_init(struct WeatherApp* app) {
    app->model = (WeatherModel*)calloc(1, sizeof(WeatherModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model alloc failed");
        return;
    }
    app->model->current_page = 0;
    weather_model_load(app);
    ESP_LOGI(TAG, "init done (city=\"%s\" use_ip=%d located=%d)",
             app->model->city, (int)app->model->use_ip, (int)app->model->located);
}

void weather_model_load(struct WeatherApp* app) {
    WeatherModel *m = app->model;
    if (!m) return;

    m->use_ip = flash_get_bool(NS, "use_ip", true);
    flash_get_str(NS, "city", m->city, sizeof(m->city), "");

    char lat_s[24], lon_s[24];
    if (flash_get_str(NS, "lat", lat_s, sizeof(lat_s), "") > 0 &&
        flash_get_str(NS, "lon", lon_s, sizeof(lon_s), "") > 0) {
        m->lat = strtod(lat_s, NULL);
        m->lon = strtod(lon_s, NULL);
        m->located = (m->lat != 0.0 || m->lon != 0.0);
    }
}

void weather_model_save_location(struct WeatherApp* app) {
    WeatherModel *m = app->model;
    if (!m) return;

    flash_set_bool(NS, "use_ip", m->use_ip);
    if (m->city[0]) flash_set_str(NS, "city", m->city);

    /* flash_store has no float API — store coordinates as strings. */
    char buf[24];
    snprintf(buf, sizeof(buf), "%.6f", m->lat);
    flash_set_str(NS, "lat", buf);
    snprintf(buf, sizeof(buf), "%.6f", m->lon);
    flash_set_str(NS, "lon", buf);
}

void weather_model_deinit(struct WeatherApp* app) {
    if (app->model) {
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
