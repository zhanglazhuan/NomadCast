#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <stdbool.h>
#include <stddef.h>
#include "model.h"   /* WEATHER_DAYS, WEATHER_CITY_MAX, WEATHER_SEARCH_MAX */

/* Parsed 7-day forecast (Open-Meteo). */
typedef struct {
    float temp;
    float feels_like;
    float humidity;   /* % */
    float wind;       /* km/h */
    char  text[32];                          /* localized weather text */
    float tmax[WEATHER_DAYS];
    float tmin[WEATHER_DAYS];
    char  daily_text[WEATHER_DAYS][32];      /* localized daily text */
    char  daily_date[WEATHER_DAYS][11];      /* "YYYY-MM-DD" */
} weather_forecast_t;

/* One geocoding result (Open-Meteo search). */
typedef struct {
    char   name[WEATHER_CITY_MAX];
    double lat;
    double lon;
} weather_geo_result_t;

/* Outcome of a weather network call — lets the UI show the specific failure
 * (no network / DNS / TLS-clock / timeout) instead of a generic "failed". */
typedef enum {
    WAPI_OK = 0,
    WAPI_ERR_CONNECT,   /* couldn't reach host (no network / DNS / refused) */
    WAPI_ERR_TLS,       /* TLS handshake failed (usually clock not synced)  */
    WAPI_ERR_TIMEOUT,   /* request timed out                                */
    WAPI_ERR_SERVER,    /* HTTP non-200                                     */
    WAPI_ERR_PARSE,     /* body received but JSON/fields invalid            */
} weather_api_err_t;

/* IP geolocation (ipwho.is). Fills lat/lon and a human-readable city label. */
weather_api_err_t weather_api_locate(double *lat, double *lon, char *city, size_t city_len);

/* Fetch current + 7-day forecast for the given coordinates. */
weather_api_err_t weather_api_forecast(double lat, double lon, weather_forecast_t *out);

/* Geocode a city name into up to `max` results (name includes country). */
weather_api_err_t weather_api_geocode(const char *query, weather_geo_result_t *results, int max, int *out_count);

#endif // WEATHER_API_H
