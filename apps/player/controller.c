#include <stdlib.h>
#include <string.h>
#include "controller.h"
#include "app.h"
#include "model.h"
#include "view.h"
#include "audio_player.h"
#include "lv_toast.h"
#include "lang.h"
#include "esp_log.h"

static const char *TAG = "player_ctrl";

void player_controller_init(struct PlayerApp* app) {
    app->controller = (PlayerController*)malloc(sizeof(PlayerController));
    if (!app->controller) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }
    app->controller->model = NULL;
    app->controller->view = NULL;
    ESP_LOGI(TAG, "init done");
}

void player_controller_deinit(struct PlayerApp* app) {
    if (app->controller) {
        free(app->controller);
        app->controller = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}

void player_controller_play_file(struct PlayerApp* app, const char* path) {
    if (!app || !app->model || !path) return;

    audio_player_init();
    bool ok = audio_player_play(path);
    if (ok) {
        strncpy(app->model->current_file, path, PLAYER_PATH_MAX - 1);
        app->model->current_file[PLAYER_PATH_MAX - 1] = '\0';
    } else {
        lv_toast_show(tr(STR_PLAYER_FAILED_TO_PLAY), 2000);
    }
}

void player_nav_push(struct PlayerApp* app, int from, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    PAGE_NAVIGATE_TO(app, from, to, NULL);
}

void player_nav_replace(struct PlayerApp* app, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    page_navigator_navigate_to(&app->view->page_nav, app, to, NULL);
}

void player_go_up(struct PlayerApp* app) {
    if (!app || !app->model) return;
    char *dir = app->model->current_dir;
    if (!dir[0] || strcmp(dir, PLAYER_SD_ROOT) == 0) {
        strcpy(dir, PLAYER_SD_ROOT);
    } else {
        char *slash = strrchr(dir, '/');
        if (slash == dir) {
            strcpy(dir, PLAYER_SD_ROOT);   /* "/foo" → root */
        } else if (slash) {
            *slash = '\0';                  /* "/a/b" → "/a" */
        }
    }
    player_nav_replace(app, PAGE_FILES);
}
