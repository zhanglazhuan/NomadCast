#ifndef WEATHER_VIEW_H
#define WEATHER_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct WeatherApp;

typedef struct WeatherView {
    page_navigator_t page_nav;
} WeatherView;

#define WEATHER_PAGE_ID_MAX 3

enum weather_page_id_t {
    PAGE_NONE = 0,
    PAGE_CURRENT,   /* 1 — current weather + 7-day forecast (root) */
    PAGE_CITY,      /* 2 — city selection (IP toggle + search) */
};

void weather_view_init(struct WeatherApp* app);
void weather_view_deinit(struct WeatherApp* app);

void weather_view_current_init_registry(struct WeatherApp* app);
void weather_view_city_init_registry(struct WeatherApp* app);

#ifdef __cplusplus
}
#endif

#endif // WEATHER_VIEW_H
