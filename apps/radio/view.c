#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "model.h"
#include "subpages/view_browse.h"
#include "subpages/view_favorites.h"
#include "subpages/view_stations.h"
#include "subpages/view_playing.h"
#include "app_event.h"
#include "esp_log.h"

static const char *TAG = "radio_view";

/* Worker task completes a network op → APP_EVENT_RADIO_UPDATED. Rebuild the
 * active page from the model. Region/station/favorite pages all render purely
 * from the model, so re-running the builder is safe. */
static void on_radio_event(app_event_t event, const void *data)
{
    (void)data;
    if (event != APP_EVENT_RADIO_UPDATED) return;

    RadioApp *app = &g_radio_app;
    if (!app || !app->view || !app->model) return;

    int pg = app->model->current_page;
    if (pg == PAGE_NONE) pg = PAGE_BROWSE;
    page_navigator_navigate_to(&app->view->page_nav, app, pg, NULL);
}

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

    radio_view_browse_init_registry(app);
    radio_view_favorites_init_registry(app);
    radio_view_stations_init_registry(app);
    radio_view_playing_init_registry(app);

    app_event_register(on_radio_event);

    ESP_LOGI(TAG, "init done (4 pages registered)");
}

void radio_view_deinit(struct RadioApp* app) {
    if (app->view) {
        app_event_unregister(on_radio_event);
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
