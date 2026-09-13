#include <stdlib.h>
#include <string.h>
#include "controller.h"
#include "app.h"
#include "model.h"
#include "view.h"
#include "esp_log.h"

static const char *TAG = "alarm_ctrl";

void alarm_controller_init(struct AlarmApp* app) {
    app->controller = (AlarmController*)malloc(sizeof(AlarmController));
    if (!app->controller) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }
    app->controller->model = NULL;
    app->controller->view  = NULL;
    ESP_LOGI(TAG, "init done");
}

void alarm_controller_deinit(struct AlarmApp* app) {
    if (app->controller) {
        free(app->controller);
        app->controller = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}

void alarm_nav_push(struct AlarmApp* app, int from, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    PAGE_NAVIGATE_TO(app, from, to, NULL);
}

void alarm_nav_replace(struct AlarmApp* app, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    page_navigator_navigate_to(&app->view->page_nav, app, to, NULL);
}
