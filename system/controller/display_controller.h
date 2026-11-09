#pragma once
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Call from the LVGL thread. The caller owns the panel and its power/reset.
 * Init applies display-off and backlight-off before the first rendered frame. */
esp_err_t display_controller_init(esp_lcd_panel_handle_t panel, gpio_num_t backlight_pin);
esp_err_t display_controller_set_backlight_pin(gpio_num_t pin);
esp_err_t display_controller_wake(void);
esp_err_t display_controller_sleep(void);
bool display_controller_is_awake(void);
#ifdef __cplusplus
}
#endif
