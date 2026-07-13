#ifndef SYS_BATTERY_H
#define SYS_BATTERY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init ADC + CHAG, take one sample, and start the 5-minute periodic sampler.
 * Fires APP_EVENT_BATTERY_CHANGED when the status-bar icon bucket or charging
 * state changes. Call after LVGL + the status bar are initialized. */
void battery_init(void);

/* Last quantized percent 0..100 (-1 before the first sample). */
int  battery_get_percent(void);

/* Last charging state (CHAG low). */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_BATTERY_H */
