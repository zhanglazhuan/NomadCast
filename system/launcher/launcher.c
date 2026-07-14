/*
 * NomadCast — App Launcher Implementation
 *
 * Adapted from EPOS/Zephyr (epos_launcher.c) to ESP-IDF.
 *
 * Dependencies:
 *   - app_manager.h  (sys/managers)
 *   - lvgl.h
 *   - esp_log.h
 */

#include "esp_log.h"
#include "lvgl.h"
#include "app_manager.h"
#include "launcher.h"

static const char *TAG = "launcher";

/* ---- App launcher ---- */

void launcher_open_app(const char *app_name)
{
    ESP_LOGI(TAG, "open_app: %s", app_name ? app_name : "(null)");

    /* root and group are NULL for now — apps create their own screens */
    app_manager_show(launcher_on_app_close, lv_screen_active(), NULL, app_name);
}

void launcher_on_app_close(void)
{
    ESP_LOGI(TAG, "App closed, returning to home");
    launcher_home_ui();
}

void launcher_return_home(void)
{
    ESP_LOGI(TAG, "Global gesture: returning to Home UI");

    /* Kill current app */
    app_manager_delete();

    /* Reload home screen */
    launcher_home_ui();
}
