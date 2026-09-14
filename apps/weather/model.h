#ifndef WEATHER_MODEL_H
#define WEATHER_MODEL_H

#include <stdbool.h>

struct WeatherApp;

#define WEATHER_CITY_MAX   64
#define WEATHER_SEARCH_MAX 8
#define WEATHER_DAYS       7

/* Shared between the LVGL thread (UI) and the worker task (network). The worker
 * writes results first and *then* fires APP_EVENT_WEATHER_UPDATED; the UI reads
 * them from the event callback (drained on the LVGL thread). The event queue's
 * send/receive ordering gives the worker's writes a happens-before edge over the
 * UI's reads, so no separate lock is required — the same pattern the podcast
 * model uses between its RSS worker and the UI. */
typedef struct WeatherModel {
    /* location */
    char   city[WEATHER_CITY_MAX];
    double lat;
    double lon;
    bool   use_ip;
    bool   located;     /* true once we have valid coordinates */

    /* status */
    bool   busy;
    bool   has_data;    /* true once a forecast has been received */
    char   error[128];
    char   updated[16]; /* "HH:MM" of the last successful fetch */

    /* current weather */
    float  temp;
    float  feels_like;
    float  humidity;    /* % */
    float  wind;        /* km/h */
    char   text[32];    /* localized weather text (QWeather) */

    /* 7-day forecast */
    float  tmax[WEATHER_DAYS];
    float  tmin[WEATHER_DAYS];
    char   daily_text[WEATHER_DAYS][32];   /* localized daily text */
    char   daily_date[WEATHER_DAYS][11];   /* "YYYY-MM-DD" */

    /* city search results */
    char   search_name[WEATHER_SEARCH_MAX][WEATHER_CITY_MAX];
    double search_lat[WEATHER_SEARCH_MAX];
    double search_lon[WEATHER_SEARCH_MAX];
    int    search_count;
    bool   search_done;  /* a search has completed at least once */
    char   search_query[WEATHER_CITY_MAX]; /* last submitted query (restored on rebuild) */

    /* navigation */
    int    current_page; /* PAGE_CURRENT / PAGE_CITY (view.h) */
} WeatherModel;

void weather_model_init(struct WeatherApp* app);
void weather_model_deinit(struct WeatherApp* app);
void weather_model_load(struct WeatherApp* app);
void weather_model_save_location(struct WeatherApp* app);

#endif // WEATHER_MODEL_H
