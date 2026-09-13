#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "subpages/view_alarm_list.h"
#include "subpages/view_alarm_edit.h"
#include "esp_log.h"

static const char *TAG = "alarm_view";

void alarm_view_init(struct AlarmApp* app) {
    app->view = (AlarmView*)malloc(sizeof(AlarmView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * ALARM_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * ALARM_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, ALARM_PAGE_ID_MAX, app);

    alarm_view_list_init_registry(app);
    alarm_view_edit_init_registry(app);

    ESP_LOGI(TAG, "init done (2 pages registered)");
}

void alarm_view_deinit(struct AlarmApp* app) {
    if (app->view) {
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
