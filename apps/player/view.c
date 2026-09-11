#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "subpages/view_files.h"
#include "subpages/view_player.h"
#include "esp_log.h"

static const char *TAG = "player_view";

void player_view_init(struct PlayerApp* app) {
    app->view = (PlayerView*)malloc(sizeof(PlayerView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * PLAYER_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * PLAYER_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, PLAYER_PAGE_ID_MAX, app);

    player_view_files_init_registry(app);
    player_view_player_init_registry(app);

    ESP_LOGI(TAG, "init done (2 pages registered)");
}

void player_view_deinit(struct PlayerApp* app) {
    if (app->view) {
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
