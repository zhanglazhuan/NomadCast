#ifndef SETTINGS_VIEW_OTA_STATUS_H
#define SETTINGS_VIEW_OTA_STATUS_H

#include <lvgl.h>

struct SettingsApp;

/* OTA 进度共享变量:-1 空闲, 0..100 进度, -2 失败。
 * 由 esp_event 任务写 (view_update.c 的 on_ota_progress),本页 LVGL 定时器读。 */
extern volatile int g_ota_progress;

void settings_view_ota_status_init_registry(struct SettingsApp* app);

#endif // SETTINGS_VIEW_OTA_STATUS_H
