#ifndef RADIO_APP_H
#define RADIO_APP_H

#include <lvgl.h>

struct RadioModel;
struct RadioView;
struct RadioController;

typedef struct RadioApp {
    struct RadioModel* model;
    struct RadioView* view;
    struct RadioController* controller;
} RadioApp;

extern RadioApp g_radio_app;

void radio_app_register(void);  /* self-register with app_manager */

#endif // RADIO_APP_H
