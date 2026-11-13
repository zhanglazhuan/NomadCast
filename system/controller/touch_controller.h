#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bus is owned by the caller; shared LCD/TP reset must precede display init.
 * Call on the LVGL thread. Polling pauses when the indev is disabled. */
esp_err_t touch_controller_init(i2c_master_bus_handle_t bus, lv_display_t *display,
                                lv_indev_t **indev);
#ifdef __cplusplus
}
#endif
