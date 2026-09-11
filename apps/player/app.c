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

PlayerApp g_player_app;

/* Icon */
extern const lv_image_dsc_t ic_player_60x60;

static const char *TAG = "player_app";

/* ---- Wrappers for app_manager ---- */

static void player_app_start(lv_obj_t *root, lv_group_t *group)
{
    (void)root; (void)group;

    player_model_init(&g_player_app);
    player_controller_init(&g_player_app);
    player_view_init(&g_player_app);

    g_player_app.controller->model = g_player_app.model;
    g_player_app.controller->view  = g_player_app.view;

    /* Default tab: Files (SD browser at /sdcard) */
    player_nav_replace(&g_player_app, PAGE_FILES);

    ESP_LOGI(TAG, "ready");
}

static void player_app_stop(void)
{
    player_controller_deinit(&g_player_app);
    player_view_deinit(&g_player_app);
    player_model_deinit(&g_player_app);
    memset(&g_player_app, 0, sizeof(PlayerApp));
}

static bool player_app_back(void)
{
    PlayerModel *m = g_player_app.model;
    if (!m) return false;

    /* In a folder below the SD root, the back key goes up one level instead
     * of closing the app. The Files page's header back button does the same. */
    if (m->current_page == PAGE_FILES &&
        m->current_dir[0] && strcmp(m->current_dir, PLAYER_SD_ROOT) != 0) {
        player_go_up(&g_player_app);
        return true;
    }

    return page_navigator_navigate_pop(&g_player_app.view->page_nav, &g_player_app);
}

/* ---- App descriptor ---- */

static application_t player_app_desc = {
    .name       = (char *)"Player",
    .icon       = &ic_player_60x60,
    .start_func = player_app_start,
    .stop_func  = player_app_stop,
    .back_func  = player_app_back,
    .factory_reset_func = NULL,
    .hidden     = false,
    .category   = APP_CATEGORY_TOOLS,
};

void player_app_register(void)
{
    app_manager_add_application(&player_app_desc);
}
