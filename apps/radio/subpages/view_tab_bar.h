#ifndef RADIO_VIEW_TAB_BAR_H
#define RADIO_VIEW_TAB_BAR_H

#include <lvgl.h>

struct RadioApp;

#define RADIO_TAB_BAR_HEIGHT 40
#define RADIO_TAB_COUNT 2

typedef enum {
    RADIO_TAB_BROWSE = 0,
    RADIO_TAB_FAVORITES,
} radio_main_tab_t;

lv_obj_t* radio_view_create_bottom_tab_bar(lv_obj_t* parent, radio_main_tab_t active);

#endif // RADIO_VIEW_TAB_BAR_H
