#ifndef SETTINGS_APP_H
#define SETTINGS_APP_H

#include <lvgl.h>

struct SettingsModel;
struct SettingsView;
struct SettingsController;

typedef struct SettingsApp {
    struct SettingsModel* model;
    struct SettingsView* view;
    struct SettingsController* controller;
} SettingsApp;

extern SettingsApp g_settings_app;

void settings_app_init(void);
void settings_app_deinit(void);
void settings_app_register(void);  /* self-register with app_manager */
bool settings_app_factory_reset(void);

#endif // SETTINGS_APP_H
