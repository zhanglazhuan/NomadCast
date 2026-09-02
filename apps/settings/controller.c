#include <stdlib.h>

#include "controller.h"
#include "app.h"
#include "model.h"
#include "esp_log.h"

static const char *TAG = "settings_ctrl";

void settings_controller_init(struct SettingsApp* app) {
    app->controller = (SettingsController*)malloc(sizeof(SettingsController));
    if (!app->controller) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }
    app->controller->model = NULL;
    app->controller->view = NULL;
    ESP_LOGI(TAG, "init done");
}

void settings_controller_deinit(struct SettingsApp* app) {
    if (app->controller) {
        free(app->controller);
        app->controller = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
