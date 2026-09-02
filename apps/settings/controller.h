#ifndef SETTINGS_CONTROLLER_H
#define SETTINGS_CONTROLLER_H

struct SettingsApp;
struct SettingsModel;
struct SettingsView;

typedef struct SettingsController {
    struct SettingsModel* model;
    struct SettingsView* view;
} SettingsController;

void settings_controller_init(struct SettingsApp* app);
void settings_controller_deinit(struct SettingsApp* app);

#endif // SETTINGS_CONTROLLER_H
