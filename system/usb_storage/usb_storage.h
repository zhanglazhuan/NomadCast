#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* All ownership/state operations run on the LVGL thread. */
esp_err_t usb_storage_init(void);
esp_err_t usb_storage_mount(void);
void usb_storage_set_callbacks(void (*prepare)(void), void (*resume)(void));
bool usb_storage_connected(void);
bool usb_storage_local_available(void);
bool usb_storage_has_card(void);
bool usb_storage_enabled(void);
bool usb_storage_busy(void);
esp_err_t usb_storage_last_error(void);
esp_err_t usb_storage_set_enabled(bool enabled);
void usb_storage_process(void);
#ifdef __cplusplus
}
#endif
