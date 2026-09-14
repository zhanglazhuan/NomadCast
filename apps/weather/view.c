#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "model.h"
#include "subpages/view_current.h"
#include "subpages/view_city.h"
#include "app_event.h"
#include "esp_log.h"

static const char *TAG = "weather_view";

/* Worker task completes a network op → APP_EVENT_WEATHER_UPDATED. Rebuild the
 * active page from the model. The city page's submitted query is preserved in
 * model->search_query and its keyboard is hidden by default, so rebuilding it
 * (via page_navigator_navigate_to, which re-runs the builder) is safe too. */
static void on_weather_event(app_event_t event, const void *data)
{
    (void)data;
    if (event != APP_EVENT_WEATHER_UPDATED) return;

    WeatherApp *app = &g_weather_app;
    if (!app || !app->view || !app->model) return;

    int pg = app->model->current_page;
    if (pg == PAGE_NONE) pg = PAGE_CURRENT;
    page_navigator_navigate_to(&app->view->page_nav, app, pg, NULL);
}

void weather_view_init(struct WeatherApp* app) {
    app->view = (WeatherView*)malloc(sizeof(WeatherView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * WEATHER_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * WEATHER_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, WEATHER_PAGE_ID_MAX, app);

    weather_view_current_init_registry(app);
    weather_view_city_init_registry(app);

    app_event_register(on_weather_event);

    ESP_LOGI(TAG, "init done (2 pages registered)");
}

void weather_view_deinit(struct WeatherApp* app) {
    if (app->view) {
        app_event_unregister(on_weather_event);
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
