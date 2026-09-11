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

RadioApp g_radio_app;

/* Icon */
extern const lv_image_dsc_t ic_radio_60x60;

static const char *TAG = "radio_app";

/* ---- Wrappers for app_manager ---- */

static void radio_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    radio_model_init(&g_radio_app);
    radio_controller_init(&g_radio_app);
    radio_view_init(&g_radio_app);

    g_radio_app.controller->model = g_radio_app.model;
    g_radio_app.controller->view  = g_radio_app.view;

    /* Default page: station list */
    radio_nav_replace(&g_radio_app, PAGE_STATIONS);

    ESP_LOGI(TAG, "ready");
}

static void radio_app_stop(void)
{
    radio_controller_deinit(&g_radio_app);
    radio_view_deinit(&g_radio_app);
    radio_model_deinit(&g_radio_app);
    memset(&g_radio_app, 0, sizeof(RadioApp));
}

static bool radio_app_back(void)
{
    if (!g_radio_app.view) return false;
    return page_navigator_navigate_pop(&g_radio_app.view->page_nav, &g_radio_app);
}

/* ---- App descriptor ---- */

static application_t radio_app_desc = {
    .name       = (char *)"Radio",
    .icon       = &ic_radio_60x60,
    .start_func = radio_app_start,
    .stop_func  = radio_app_stop,
    .back_func  = radio_app_back,
    .factory_reset_func = NULL,
    .hidden     = false,
    .category   = APP_CATEGORY_TOOLS,
};

void radio_app_register(void)
{
    app_manager_add_application(&radio_app_desc);
}
