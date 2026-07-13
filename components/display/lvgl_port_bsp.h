#ifndef LVGL_PORT_BSP_H
#define LVGL_PORT_BSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Lvgl_PortInit(void);
void Lcd_SetBacklight(uint8_t brig);

bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
