#ifndef NOMADCAST_DISPLAY_H
#define NOMADCAST_DISPLAY_H
#include <stddef.h>
#include "esp_lcd_panel_ops.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Enable LCD power and backlight with the delays verified in t_display.
 * Caller must reset the shared LCD/CST836U reset line before panel init. */
void nomadcast_display_power_on(void);
/* Create the tested ST7789 panel; leave display off until the UI is ready.
 * max_transfer_bytes must cover the caller's largest flush buffer. */
esp_lcd_panel_handle_t nomadcast_display_init(size_t max_transfer_bytes);
#ifdef __cplusplus
}
#endif
#endif
