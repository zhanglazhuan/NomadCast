#ifndef RADIO_API_H
#define RADIO_API_H

#include "model.h"   /* radio_region_t, radio_station_t */

/* Outcome of a radio API call — lets the UI show a specific failure. */
typedef enum {
    RAPI_OK = 0,
    RAPI_ERR_CONNECT,   /* couldn't reach host (no network / DNS / refused) */
    RAPI_ERR_SERVER,    /* HTTP non-200 */
    RAPI_ERR_PARSE,     /* body received but JSON invalid */
} radio_api_err_t;

/* Fetch regions (provinces/states) for a country ("CN"/"US"). */
radio_api_err_t radio_api_regions(const char *country,
                                  radio_region_t *out, int max, int *out_count);

/* Fetch the station list for a region (order=popular, one page). */
radio_api_err_t radio_api_stations(const char *country, const char *region,
                                   radio_station_t *out, int max,
                                   int *out_count, int *out_total);

#endif // RADIO_API_H
