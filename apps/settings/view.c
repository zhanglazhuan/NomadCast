#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "subpages/view_main.h"
#include "subpages/view_general.h"
#include "subpages/view_wifi.h"
#include "subpages/view_storage.h"
#include "subpages/view_update.h"
#include "subpages/view_wifi_connect.h"
#include "subpages/view_about.h"
#include "esp_log.h"

static const char *TAG = "settings_view";

extern SettingsApp g_settings_app;

void settings_view_init(struct SettingsApp* app) {
    app->view = (SettingsView*)malloc(sizeof(SettingsView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * SETTINGS_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * SETTINGS_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, SETTINGS_PAGE_ID_MAX, app);

    settings_view_main_init_registry(app);
    settings_view_general_init_registry(app);
    settings_view_wifi_init_registry(app);
    settings_view_storage_init_registry(app);
    settings_view_update_init_registry(app);
    settings_view_wifi_connect_init_registry(app);
    settings_view_about_init_registry(app);

    ESP_LOGI(TAG, "init done (7 pages registered)");
}

void settings_view_deinit(struct SettingsApp* app) {
    if (app->view) {
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
