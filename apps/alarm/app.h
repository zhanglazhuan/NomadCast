#ifndef ALARM_APP_H
#define ALARM_APP_H

#include <lvgl.h>

struct AlarmModel;
struct AlarmView;
struct AlarmController;

typedef struct AlarmApp {
    struct AlarmModel* model;
    struct AlarmView* view;
    struct AlarmController* controller;
} AlarmApp;

extern AlarmApp g_alarm_app;

void alarm_app_register(void);  /* self-register with app_manager */

#endif // ALARM_APP_H
