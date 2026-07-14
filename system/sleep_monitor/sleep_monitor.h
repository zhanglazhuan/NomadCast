/*
 * NomadCast — Sleep / Idle Monitor
 *
 * Tracks user inactivity (touch + keys). After a configurable timeout with no
 * input, blanks the display and disables touch sensing. Only the Power button
 * short-press wakes the device back up.
 */

#pragma once

#include <stdint.h>
#include "lvgl.h"
#include "esp_lcd_types.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the sleep monitor.
 *
 * Must be called after LVGL, display, touch indev, and input are ready.
 *
 * @param panel       ST7789 panel handle (for disp_on_off to blank/restore).
 * @param touch_indev LVGL touch input device (for enable/disable).
 * @param timeout_min Initial timeout in minutes. 0 = never sleep.
 */
void sleep_monitor_init(esp_lcd_panel_handle_t panel,
                        lv_indev_t             *touch_indev,
                        int                     timeout_min);

/**
 * @brief Set the EN_POWER GPIO pin. When set (not GPIO_NUM_NC), sleep will
 *        pull this pin LOW to cut peripheral power, and wake pulls it HIGH.
 */
void sleep_monitor_set_power_pin(gpio_num_t pin);

/**
 * @brief Register a callback for re-initializing hardware after wake.
 *
 * Called after power is restored and before touch is re-enabled.
 * Use this to re-init display and touch drivers that lost state.
 */
typedef void (*sleep_monitor_wake_cb_t)(void);
void sleep_monitor_set_wake_callback(sleep_monitor_wake_cb_t cb);

/**
 * @brief Update the sleep timeout at runtime (e.g. from Settings UI).
 * @param timeout_min  0 = never sleep, otherwise 1..60 minutes.
 */
void sleep_monitor_set_timeout(int timeout_min);

/**
 * @brief Get the current timeout value (minutes).
 */
int sleep_monitor_get_timeout(void);

/**
 * @brief Notify the sleep monitor of user activity, resetting the idle timer.
 *
 * Called automatically via touch LV_EVENT_PRESSED and input_subscribe.
 * Apps may also call this directly for custom activity sources.
 */
void sleep_monitor_notify_activity(void);

/** True if the device is currently sleeping (display off, touch disabled). */
bool sleep_monitor_is_sleeping(void);

#ifdef __cplusplus
}
#endif
