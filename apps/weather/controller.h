#ifndef WEATHER_CONTROLLER_H
#define WEATHER_CONTROLLER_H

#include <stdbool.h>

struct WeatherApp;
struct WeatherModel;
struct WeatherView;

typedef struct WeatherController {
    struct WeatherModel* model;
    struct WeatherView* view;
    void* queue;   /* FreeRTOS QueueHandle_t */
    void* task;    /* FreeRTOS TaskHandle_t */
    bool  running;
} WeatherController;

void weather_controller_init(struct WeatherApp* app);
void weather_controller_deinit(struct WeatherApp* app);

/* IP locate if not yet located, otherwise just fetch the forecast. */
void weather_controller_auto_refresh(struct WeatherApp* app);

/* Fetch the forecast for the current (stored) coordinates. */
void weather_controller_refresh(struct WeatherApp* app);

/* Re-locate via IP (used when the auto-locate switch is turned on). */
void weather_controller_locate(struct WeatherApp* app);

/* Geocode a city name → results land in model->search_*. */
void weather_controller_search(struct WeatherApp* app, const char* query);

/* Apply a search result as the selected city and fetch its weather. */
void weather_controller_apply_city(struct WeatherApp* app, int idx);

/* Navigation helpers — track the active page in the model. */
void weather_nav_push(struct WeatherApp* app, int from, int to);
void weather_nav_replace(struct WeatherApp* app, int to);

#endif // WEATHER_CONTROLLER_H
