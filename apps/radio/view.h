#ifndef RADIO_VIEW_H
#define RADIO_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RadioApp;

typedef struct RadioView {
    page_navigator_t page_nav;
} RadioView;

#define RADIO_PAGE_ID_MAX 3

enum radio_page_id_t {
    PAGE_NONE = 0,
    PAGE_STATIONS,   /* 1 — station list (root) */
    PAGE_PLAYING,    /* 2 — now playing */
};

void radio_view_init(struct RadioApp* app);
void radio_view_deinit(struct RadioApp* app);

void radio_view_stations_init_registry(struct RadioApp* app);
void radio_view_playing_init_registry(struct RadioApp* app);

#ifdef __cplusplus
}
#endif

#endif // RADIO_VIEW_H
