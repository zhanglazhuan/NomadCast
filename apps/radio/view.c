#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "subpages/view_stations.h"
#include "subpages/view_playing.h"
#include "esp_log.h"

static const char *TAG = "radio_view";

void radio_view_init(struct RadioApp* app) {
    app->view = (RadioView*)malloc(sizeof(RadioView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * RADIO_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * RADIO_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, RADIO_PAGE_ID_MAX, app);

    radio_view_stations_init_registry(app);
    radio_view_playing_init_registry(app);

    ESP_LOGI(TAG, "init done (2 pages registered)");
}

void radio_view_deinit(struct RadioApp* app) {
    if (app->view) {
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
