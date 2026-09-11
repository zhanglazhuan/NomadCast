#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "app.h"
#include "esp_log.h"

static const char *TAG = "player_model";

void player_model_init(struct PlayerApp* app) {
    app->model = (PlayerModel*)calloc(1, sizeof(PlayerModel));
    if (!app->model) {
        ESP_LOGE(TAG, "model alloc failed");
        return;
    }
    strcpy(app->model->current_dir, PLAYER_SD_ROOT);
    app->model->current_page = 0;
    ESP_LOGI(TAG, "init done");
}

void player_model_deinit(struct PlayerApp* app) {
    if (app->model) {
        free(app->model);
        app->model = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
