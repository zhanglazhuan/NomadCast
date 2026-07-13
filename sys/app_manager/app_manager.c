/*
 * NomadCast — Application Manager Implementation
 *
 * Adapted from EPOS/Zephyr (epos_app_manager.c) to ESP-IDF + FreeRTOS.
 *
 * Changes from Zephyr original:
 *   - zephyr/kernel.h, zephyr/init.h    → freertos/FreeRTOS.h, freertos/task.h
 *   - zephyr/logging/log.h              → esp_log.h
 *   - LOG_MODULE_REGISTER / LOG_*       → static const char *TAG / ESP_LOG*
 *   - __ASSERT / __ASSERT_NO_MSG        → assert
 *   - SYS_INIT(..., POST_KERNEL, ...)   → explicit app_manager_init() call
 *   - All app-management logic unchanged
 */

#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "app_manager.h"

static const char *TAG = "app_mgr";

#define MAX_APPS        25
#define INVALID_APP_ID  0xFF

static SemaphoreHandle_t s_mutex;

static void async_app_start(lv_timer_t *timer);
static void async_app_close(lv_timer_t *timer);
static __attribute__((unused)) void transition_app_to_ui_hidden(application_t *app);
static __attribute__((unused)) void transition_app_to_ui_visible(application_t *app);

static application_t *apps[MAX_APPS];
static uint8_t  num_apps;
static uint8_t  num_visible_apps;
static uint8_t  current_app;
static lv_obj_t *root_obj;
static lv_group_t *group_obj;
static on_app_manager_close_fn close_cb_func;
static lv_timer_t *async_app_start_timer;
static lv_timer_t *async_app_close_timer;
static bool screen_is_on = true;

/* ========================================================================
 * Async start/close (LVGL timers, platform-agnostic)
 * ======================================================================== */

static void async_app_start(lv_timer_t *timer)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    async_app_start_timer = NULL;
    ESP_LOGI(TAG, "Start app id=%d", current_app);

    application_t *app = apps[current_app];
    assert(screen_is_on);
    app->current_state = APP_STATE_UI_VISIBLE;

    /* Release lock before calling app code to avoid deadlock */
    xSemaphoreGive(s_mutex);
    app->start_func(root_obj, group_obj);
}

static void async_app_close(lv_timer_t *timer)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t app_id = current_app;
    bool has_app = app_id < num_apps;
    xSemaphoreGive(s_mutex);

    if (has_app) {
        ESP_LOGD(TAG, "Stop app id=%d", app_id);
        bool back_consumed = false;
        if (apps[app_id]->back_func) {
            back_consumed = apps[app_id]->back_func();
        }

        if (!back_consumed) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            apps[app_id]->current_state = APP_STATE_STOPPED;
            current_app = INVALID_APP_ID;
            xSemaphoreGive(s_mutex);

            apps[app_id]->stop_func();
            app_manager_delete();
            if (close_cb_func) close_cb_func();
        }
    } else {
        ESP_LOGD(TAG, "Exit application manager");
        app_manager_delete();
        if (close_cb_func) close_cb_func();
    }
    async_app_close_timer = NULL;
}

/* ========================================================================
 * UI visibility transitions
 * ======================================================================== */

static __attribute__((unused)) void transition_app_to_ui_hidden(application_t *app)
{
    if (app && app->current_state == APP_STATE_UI_VISIBLE) {
        app->current_state = APP_STATE_UI_HIDDEN;
        ESP_LOGD(TAG, "App '%s' UI now hidden", app->name);

        if (app->ui_unavailable_func) {
            app->ui_unavailable_func();
        }
    }
}

static __attribute__((unused)) void transition_app_to_ui_visible(application_t *app)
{
    if (app && app->current_state == APP_STATE_UI_HIDDEN) {
        app->current_state = APP_STATE_UI_VISIBLE;
        ESP_LOGD(TAG, "App '%s' UI now visible", app->name);

        if (app->ui_available_func) {
            app->ui_available_func();
        }
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int app_manager_show(on_app_manager_close_fn close_cb, lv_obj_t *root,
                     lv_group_t *group, const char *app_name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool app_found = false;
    close_cb_func = close_cb;
    root_obj = root;
    group_obj = group;

    if (app_name != NULL) {
        for (int i = 0; i < num_apps; i++) {
            if (strcmp(apps[i]->name, app_name) == 0) {
                current_app = i;
                app_found = true;
                if (async_app_start_timer == NULL) {
                    async_app_start_timer = lv_timer_create(async_app_start, 1, NULL);
                    lv_timer_set_repeat_count(async_app_start_timer, 1);
                }
                break;
            }
        }
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "show: %s → %s", app_name ? app_name : "(null)", app_found ? "found" : "not found");
    return app_found ? 0 : -1;
}

void app_manager_delete(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (current_app < num_apps) {
        uint8_t id = current_app;
        apps[id]->current_state = APP_STATE_STOPPED;
        current_app = INVALID_APP_ID;
        xSemaphoreGive(s_mutex);
        apps[id]->stop_func();
        return;
    }
    xSemaphoreGive(s_mutex);
}

void app_manager_add_application(application_t *app)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    assert(num_apps < MAX_APPS);

    app->current_state = APP_STATE_STOPPED;
    apps[num_apps] = app;
    num_apps++;

    if (!app->hidden) {
        app->private_list_index = num_visible_apps;
        num_visible_apps++;
    }
    xSemaphoreGive(s_mutex);
}

void app_manager_exit_app(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (async_app_close_timer == NULL) {
        async_app_close_timer = lv_timer_create(async_app_close, 1, NULL);
        lv_timer_set_repeat_count(async_app_close_timer, 1);
    }
    xSemaphoreGive(s_mutex);
}

void app_manager_app_close_request(application_t *app)
{
    (void)app;
    app_manager_exit_app();
}

int app_manager_get_num_apps(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = num_apps;
    xSemaphoreGive(s_mutex);
    return n;
}

application_t *app_manager_get_app(int index)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    application_t *app = (index >= 0 && index < num_apps) ? apps[index] : NULL;
    xSemaphoreGive(s_mutex);
    return app;
}

app_state_t app_manager_get_app_state(application_t *app)
{
    assert(app != NULL);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    app_state_t s = app->current_state;
    xSemaphoreGive(s_mutex);
    return s;
}

bool app_manager_is_app_running(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool running = current_app < num_apps;
    xSemaphoreGive(s_mutex);
    return running;
}

const char *app_manager_get_current_app_name(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const char *name = NULL;
    if (current_app < num_apps && apps[current_app] != NULL) {
        name = apps[current_app]->name;
    }
    xSemaphoreGive(s_mutex);
    return name;
}

void app_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(apps, 0, sizeof(apps));
    num_apps = 0;
    current_app = INVALID_APP_ID;
    async_app_start_timer = NULL;
    screen_is_on = true;

    ESP_LOGI(TAG, "Initialized (max %d apps)", MAX_APPS);
}
