#ifndef RADIO_MODEL_H
#define RADIO_MODEL_H

#include <stdbool.h>

struct RadioApp;

#define RADIO_STATION_NAME_MAX 64
#define RADIO_STATION_URL_MAX  256

typedef struct {
    char name[RADIO_STATION_NAME_MAX];
    char url[RADIO_STATION_URL_MAX];
} radio_station_t;

typedef struct RadioModel {
    const radio_station_t *stations;  /* preset table (const, in flash) */
    int count;
    int current;                       /* index of playing station, -1 = none */
    int current_page;                  /* PAGE_STATIONS / PAGE_PLAYING (view.h) */
} RadioModel;

void radio_model_init(struct RadioApp* app);
void radio_model_deinit(struct RadioApp* app);

#endif // RADIO_MODEL_H
