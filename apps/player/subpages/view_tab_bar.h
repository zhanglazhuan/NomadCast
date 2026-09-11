#ifndef PLAYER_VIEW_TAB_BAR_H
#define PLAYER_VIEW_TAB_BAR_H

#include <lvgl.h>

struct PlayerApp;

#define PLAYER_TAB_BAR_HEIGHT 40
#define PLAYER_TAB_COUNT 2

typedef enum {
    PLAYER_TAB_FILES = 0,
    PLAYER_TAB_PLAYER,
} player_main_tab_t;

lv_obj_t* player_view_create_bottom_tab_bar(lv_obj_t* parent, player_main_tab_t active);

#endif // PLAYER_VIEW_TAB_BAR_H
