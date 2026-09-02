#ifndef SETTINGS_VIEW_H
#define SETTINGS_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SettingsApp;

typedef struct SettingsView {
    page_navigator_t page_nav;
} SettingsView;

#define SETTINGS_PAGE_ID_MAX 9

enum settings_page_id_t {
    SETTINGS_PAGE_NONE = 0,
    SETTINGS_PAGE_MAIN,         // 1 — 设置主页
    SETTINGS_PAGE_GENERAL,      // 2 — General
    SETTINGS_PAGE_WIFI,         // 3 — WIFI
    SETTINGS_PAGE_STORAGE,      // 4 — Storage
    SETTINGS_PAGE_UPDATE,       // 5 — Update
    SETTINGS_PAGE_WIFI_CONNECT, // 6 — WiFi 连接页
    SETTINGS_PAGE_ABOUT,        // 7 — 关于本机
    SETTINGS_PAGE_OTA_STATUS,   // 8 — OTA 状态页
};

void settings_view_init(struct SettingsApp* app);
void settings_view_deinit(struct SettingsApp* app);

// 子页面注册
void settings_view_main_init_registry(struct SettingsApp* app);
void settings_view_general_init_registry(struct SettingsApp* app);
void settings_view_wifi_init_registry(struct SettingsApp* app);
void settings_view_storage_init_registry(struct SettingsApp* app);
void settings_view_update_init_registry(struct SettingsApp* app);
void settings_view_wifi_connect_init_registry(struct SettingsApp* app);
void settings_view_about_init_registry(struct SettingsApp* app);
void settings_view_ota_status_init_registry(struct SettingsApp* app);

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_VIEW_H
