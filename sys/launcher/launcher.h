/*
 * NomadCast — App Launcher (adapted from EPOS/Zephyr)
 *
 * Three parts:
 *   launcher.h        — Public API (open app, close callback, return home)
 *   launcher_home_ui.c — Home screen with app icon grid
 *   launcher_gesture.c — Global swipe gestures (exit app, control panel)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Open an app by name (called from home UI grid). */
void launcher_open_app(const char *app_name);

/** @brief Callback when an app closes — returns to home UI. */
void launcher_on_app_close(void);

/** @brief Global gesture: return to home, killing current app. */
void launcher_return_home(void);

/** @brief Initialize global swipe gestures (call once after LVGL indev is ready). */
void launcher_gesture_init(lv_indev_t *touch_indev);

/** @brief Show the home screen (app icon grid). */
void launcher_home_ui(void);

#ifdef __cplusplus
}
#endif
