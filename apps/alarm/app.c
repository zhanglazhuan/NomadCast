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
#include "alarm_service.h"

AlarmApp g_alarm_app;

/* Icon */
extern const lv_image_dsc_t ic_alarm_60x60;

static const char *TAG = "alarm_app";

/* ---- Wrappers for app_manager ---- */

static void alarm_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    alarm_model_init(&g_alarm_app);
    alarm_controller_init(&g_alarm_app);
    alarm_view_init(&g_alarm_app);

    g_alarm_app.controller->model = g_alarm_app.model;
    g_alarm_app.controller->view  = g_alarm_app.view;

    /* Default page: alarm list */
    alarm_nav_replace(&g_alarm_app, PAGE_LIST);

    ESP_LOGI(TAG, "ready");
}

static void alarm_app_stop(void)
{
    alarm_controller_deinit(&g_alarm_app);
    alarm_view_deinit(&g_alarm_app);
    alarm_model_deinit(&g_alarm_app);
    memset(&g_alarm_app, 0, sizeof(AlarmApp));
}

static bool alarm_app_back(void)
{
    if (!g_alarm_app.view) return false;
    return page_navigator_navigate_pop(&g_alarm_app.view->page_nav, &g_alarm_app);
}

static bool alarm_app_factory_reset(void)
{
    alarm_service_reset();
    return true;
}

/* ---- App descriptor ---- */

static application_t alarm_app_desc = {
    .name       = (char *)"Alarm",
    .icon       = &ic_alarm_60x60,
    .start_func = alarm_app_start,
    .stop_func  = alarm_app_stop,
    .back_func  = alarm_app_back,
    .factory_reset_func = alarm_app_factory_reset,
    .hidden     = false,
    .category   = APP_CATEGORY_TOOLS,
};

void alarm_app_register(void)
{
    app_manager_add_application(&alarm_app_desc);
}
