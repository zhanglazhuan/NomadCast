#ifndef RADIO_CONTROLLER_H
#define RADIO_CONTROLLER_H

#include <stdbool.h>
#include "model.h"

struct RadioApp;
struct RadioModel;
struct RadioView;

typedef struct RadioController {
    struct RadioModel* model;
    struct RadioView* view;
    void* queue;   /* FreeRTOS QueueHandle_t */
    void* task;    /* FreeRTOS TaskHandle_t */
    bool  running;
} RadioController;

void radio_controller_init(struct RadioApp* app);
void radio_controller_deinit(struct RadioApp* app);

/* Fetch regions for the current country. */
void radio_controller_load_regions(struct RadioApp* app);

/* Fetch the station list for a region (sets model region context). */
void radio_controller_load_stations(struct RadioApp* app,
                                    const char *region_key, const char *region_name);

/* Play a station from a given list (stations[] or favorites[]). */
void radio_controller_play(struct RadioApp* app, int src, int idx);

/* Toggle play/pause for the current stream. */
void radio_controller_toggle(struct RadioApp* app);

/* Next / previous station in the active list (wrap-around). */
void radio_controller_next(struct RadioApp* app);
void radio_controller_prev(struct RadioApp* app);

/* Toggle the current station's favorite state (persists to NVS). */
void radio_controller_toggle_favorite(struct RadioApp* app);

/* Navigation helpers — track the active page in the model. */
void radio_nav_push(struct RadioApp* app, int from, int to);
void radio_nav_replace(struct RadioApp* app, int to);

#endif // RADIO_CONTROLLER_H
