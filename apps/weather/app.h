#ifndef WEATHER_APP_H
#define WEATHER_APP_H

#include <lvgl.h>

struct WeatherModel;
struct WeatherView;
struct WeatherController;

typedef struct WeatherApp {
    struct WeatherModel* model;
    struct WeatherView* view;
    struct WeatherController* controller;
} WeatherApp;

extern WeatherApp g_weather_app;

void weather_app_register(void);  /* self-register with app_manager */

#endif // WEATHER_APP_H
