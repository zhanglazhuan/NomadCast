#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "app_manager.h"

#include "app.h"
#include "view.h"
#include "controller.h"
#include "model.h"

SettingsApp g_settings_app;

/* Icon */
extern const lv_image_dsc_t ic_settings_40x40;

/* ---- Wrappers for app_manager ---- */

static void settings_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    settings_model_init(&g_settings_app);
    settings_controller_init(&g_settings_app);
    settings_view_init(&g_settings_app);

    g_settings_app.controller->model = g_settings_app.model;
    g_settings_app.controller->view  = g_settings_app.view;

    /* Navigate to main page */
    PAGE_NAVIGATE_TO((&g_settings_app), SETTINGS_PAGE_NONE, SETTINGS_PAGE_MAIN, NULL);
}

static void settings_app_stop(void)
{
    settings_controller_deinit(&g_settings_app);
    settings_view_deinit(&g_settings_app);
    settings_model_deinit(&g_settings_app);
    memset(&g_settings_app, 0, sizeof(SettingsApp));
}

static bool settings_app_back(void)
{
    return page_navigator_navigate_pop(&g_settings_app.view->page_nav, &g_settings_app);
}

/* ---- App descriptor ---- */

static application_t settings_app_desc = {
    .name       = (char *)"Settings",
    .icon       = &ic_settings_40x40,
    .start_func = settings_app_start,
    .stop_func  = settings_app_stop,
    .back_func  = settings_app_back,
    .hidden     = false,
    .category   = APP_CATEGORY_SYSTEM,
};

void settings_app_register(void)
{
    app_manager_add_application(&settings_app_desc);
}

/* Legacy API (kept for backward compat) */
void settings_app_init(void)  { /* use register + start_func instead */ }
void settings_app_deinit(void) { /* use stop_func instead */ }
