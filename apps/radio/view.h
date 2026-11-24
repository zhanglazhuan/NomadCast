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

#define RADIO_PAGE_ID_MAX 5

enum radio_page_id_t {
    PAGE_NONE = 0,
    PAGE_BROWSE,      /* 1 — country + region list (tab: 电台) */
    PAGE_FAVORITES,   /* 2 — favorites list (tab: 收藏) */
    PAGE_STATIONS,    /* 3 — station list for a region */
    PAGE_PLAYING,     /* 4 — now playing */
};

void radio_view_init(struct RadioApp* app);
void radio_view_deinit(struct RadioApp* app);

/* Offline hint: "无网络" + "打开设置" button (same as Podcast). Builds into
 * `parent` as floating widgets; tapping the button closes the app and opens
 * Settings. */
void radio_view_wifi_offline(lv_obj_t *parent);

void radio_view_browse_init_registry(struct RadioApp* app);
void radio_view_favorites_init_registry(struct RadioApp* app);
void radio_view_stations_init_registry(struct RadioApp* app);
void radio_view_playing_init_registry(struct RadioApp* app);

#ifdef __cplusplus
}
#endif

#endif // RADIO_VIEW_H
