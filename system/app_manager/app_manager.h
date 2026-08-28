/*
 * NomadCast — Application Manager (adapted from EPOS/Zephyr)
 *
 * Manages the lifecycle of LVGL-based applications:
 *   - Register apps with name, callbacks, category
 *   - Start/stop apps by name
 *   - UI visibility state machine (visible ↔ hidden)
 *   - Async start/close via LVGL timers
 *
 * Original: D:\Codes\EPOS\epos\managers\epos_app_manager.c (Zephyr)
 * Ported to: ESP-IDF + FreeRTOS
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Types
 * ======================================================================== */

typedef void (*app_start_fn)(lv_obj_t *root, lv_group_t *group);
typedef void (*app_stop_fn)(void);
typedef bool (*app_back_fn)(void);               /* true = back consumed */
typedef bool (*app_factory_reset_fn)(void);
typedef void (*app_ui_unavailable_fn)(void);
typedef void (*app_ui_available_fn)(void);
typedef void (*on_app_manager_close_fn)(void);

typedef enum {
    APP_CATEGORY_ROOT = 0,
    APP_CATEGORY_TOOLS,
    APP_CATEGORY_FITNESS,
    APP_CATEGORY_SYSTEM,
    APP_CATEGORY_GAMES,
    APP_CATEGORY_SENSORS,
    APP_CATEGORY_RANDOM,
    APP_CATEGORY_COUNT,
    APP_CATEGORY_INVALID
} app_category_t;

typedef enum {
    APP_STATE_STOPPED,       /* App is not running */
    APP_STATE_UI_VISIBLE,    /* App UI is visible and safe to use */
    APP_STATE_UI_HIDDEN      /* App is running but UI is not safe to call */
} app_state_t;

typedef struct application_t {
    app_start_fn            start_func;
    app_stop_fn             stop_func;
    app_back_fn             back_func;
    app_factory_reset_fn    factory_reset_func;
    app_ui_unavailable_fn   ui_unavailable_func;
    app_ui_available_fn     ui_available_func;
    char                   *name;
    const void             *icon;
    bool                    hidden;
    app_category_t          category;
    uint8_t                 private_list_index;
    app_state_t             current_state;
} application_t;

/* ========================================================================
 * Public API
 * ======================================================================== */

/** @brief Initialize the application manager. Call once at boot. */
void app_manager_init(void);

/**
 * @brief Register an application.
 * @param app  Pointer to statically-allocated application_t (must persist).
 */
void app_manager_add_application(application_t *app);

/** Invoke each registered app's user-data factory reset callback. */
bool app_manager_factory_reset_all(void);

/**
 * @brief Start an application by name.
 * @param close_cb  Called when the app closes.
 * @param root      LVGL root object passed to app's start_func.
 * @param group     LVGL group passed to app's start_func.
 * @param app_name  Name of the app to launch.
 * @return 0 on success, -1 if not found.
 */
int app_manager_show(on_app_manager_close_fn close_cb, lv_obj_t *root,
                     lv_group_t *group, const char *app_name);

/** @brief Force-stop the currently running app. */
void app_manager_delete(void);

/** @brief Request the current app to close (via its back_func). */
void app_manager_exit_app(void);

/** @brief Public close request (same as exit). */
void app_manager_app_close_request(application_t *app);

/** @brief Get the number of registered apps. */
int app_manager_get_num_apps(void);

/** @brief Get an app by its registration index. */
application_t *app_manager_get_app(int index);

/** @brief Get the current state of an app. */
app_state_t app_manager_get_app_state(application_t *app);

/** @brief Check if any app is currently running. */
bool app_manager_is_app_running(void);

/** @brief Get the name of the currently running app, or NULL. */
const char *app_manager_get_current_app_name(void);

#ifdef __cplusplus
}
#endif
