#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "app.h"
#include "esp_log.h"

static const char *TAG = "alarm_model";

void alarm_model_init(struct AlarmApp* app) {
    app->model = (AlarmModel*)calloc(1, sizeof(AlarmModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model alloc failed");
        return;
    }
    app->model->current_page  = 0;
    app->model->editing_index = -1;
    ESP_LOGI(TAG, "init done");
}

void alarm_model_deinit(struct AlarmApp* app) {
    if (app->model) {
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
