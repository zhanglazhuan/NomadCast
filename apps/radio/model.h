#ifndef RADIO_MODEL_H
#define RADIO_MODEL_H

#include <stdbool.h>

struct RadioApp;

#define RADIO_NAME_MAX        64
#define RADIO_UUID_MAX        40
#define RADIO_REGION_KEY_MAX  32
#define RADIO_REGION_NAME_MAX 64
#define RADIO_FAVICON_MAX     160
#define RADIO_CODEC_MAX       8

#define RADIO_MAX_REGIONS    64   /* CN ~34 provinces, US ~51 states */
#define RADIO_MAX_STATIONS   100  /* one page of stations per region */
#define RADIO_MAX_FAVORITES  32

/* One radio station (from /api/radio/stations or a saved favorite). */
typedef struct {
    char uuid[RADIO_UUID_MAX];
    char name[RADIO_NAME_MAX];
    char region[RADIO_REGION_KEY_MAX];
    char region_name[RADIO_REGION_NAME_MAX];
    char codec[RADIO_CODEC_MAX];
    char favicon[RADIO_FAVICON_MAX];
    int  votes;
    int  bitrate;
    bool hls;
} radio_station_t;

/* One region (province/state) from /api/radio/regions. */
typedef struct {
    char key[RADIO_REGION_KEY_MAX];
    char name[RADIO_REGION_NAME_MAX];
    int  order;
    int  count;
} radio_region_t;

/* Which list the playing station belongs to (drives next/prev wrap-around). */
typedef enum {
    RADIO_SRC_STATIONS = 0,
    RADIO_SRC_FAVORITES,
} radio_src_t;

/* Shared between the LVGL thread (UI) and the worker task (network). The worker
 * writes results first and *then* fires APP_EVENT_RADIO_UPDATED; the UI reads
 * them from the event callback (drained on the LVGL thread). The event queue's
 * send/receive ordering gives the worker's writes a happens-before edge over the
 * UI's reads — the same pattern the weather model uses. */
typedef struct RadioModel {
    /* country ("CN" / "US") — derived from the system region setting, not
     * user-selectable inside the radio app. */
    char country[4];

    /* regions (browse list) */
    radio_region_t regions[RADIO_MAX_REGIONS];
    int  region_count;

    /* stations (currently selected region) */
    char region_key[RADIO_REGION_KEY_MAX];
    char region_name[RADIO_REGION_NAME_MAX];
    radio_station_t stations[RADIO_MAX_STATIONS];
    int  station_count;
    int  station_total;   /* total on the server, may exceed the loaded page */

    /* favorites (persisted to NVS) */
    radio_station_t favorites[RADIO_MAX_FAVORITES];
    int  favorite_count;

    /* now playing */
    int  current;         /* index into stations[] or favorites[] */
    int  play_source;     /* radio_src_t */

    /* status */
    bool busy;
    char error[128];

    /* navigation + search */
    int  current_page;    /* PAGE_* (view.h) */
    char search_query[RADIO_REGION_NAME_MAX];
} RadioModel;

void radio_model_init(struct RadioApp* app);
void radio_model_deinit(struct RadioApp* app);

/* Favorite persistence + lookup. */
void radio_model_load_favorites(struct RadioApp* app);
void radio_model_save_favorites(struct RadioApp* app);
bool radio_model_is_favorite(struct RadioApp* app, const char *uuid);
/* Add if absent / remove if present. Returns true when now favorited. */
bool radio_model_toggle_favorite(struct RadioApp* app, const radio_station_t *s);

#endif // RADIO_MODEL_H
