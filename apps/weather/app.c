#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_log.h"
#include "app_manager.h"
#include "page_navigator.h"

#include "app.h"
#include "view.h"
#include "controller.h"
#include "model.h"

WeatherApp g_weather_app;

/* Icon */
extern const lv_image_dsc_t ic_weather_60x60;

static const char *TAG = "weather_app";

/* ---- Wrappers for app_manager ---- */

static void weather_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    weather_model_init(&g_weather_app);
    weather_controller_init(&g_weather_app);
    weather_view_init(&g_weather_app);

    g_weather_app.controller->model = g_weather_app.model;
    g_weather_app.controller->view  = g_weather_app.view;

    /* Default page: current weather */
    weather_nav_replace(&g_weather_app, PAGE_CURRENT);

    /* Kick off the first fetch: IP locate (if enabled) or stored coordinates. */
    weather_controller_auto_refresh(&g_weather_app);

    ESP_LOGI(TAG, "ready");
}

static void weather_app_stop(void)
{
    weather_controller_deinit(&g_weather_app);
    weather_view_deinit(&g_weather_app);
    weather_model_deinit(&g_weather_app);
    memset(&g_weather_app, 0, sizeof(WeatherApp));
}

static bool weather_app_back(void)
{
    if (!g_weather_app.view) return false;
    return page_navigator_navigate_pop(&g_weather_app.view->page_nav, &g_weather_app);
}

/* ---- App descriptor ---- */

static application_t weather_app_desc = {
    .name       = (char *)"Weather",
    .icon       = &ic_weather_60x60,
    .start_func = weather_app_start,
    .stop_func  = weather_app_stop,
    .back_func  = weather_app_back,
    .factory_reset_func = NULL,
    .hidden     = false,
    .category   = APP_CATEGORY_TOOLS,
};

void weather_app_register(void)
{
    app_manager_add_application(&weather_app_desc);
}
