#ifndef PODCAST_VIEW_TAB_BAR_H
#define PODCAST_VIEW_TAB_BAR_H

#include <lvgl.h>

struct PodcastApp;

// 底部 4 Tab 常量
#define TAB_BAR_HEIGHT 40
#define TAB_COUNT 4

typedef enum {
    TAB_NETWORK = 0,
    TAB_LOCAL,
    TAB_PLAYER,
    TAB_PROFILE,
} main_tab_t;

/**
 * @brief 创建底部导航栏
 * @param parent  父对象
 * @param active  当前激活的 tab
 * @return 底部栏对象
 */
lv_obj_t* podcast_view_create_bottom_tab_bar(lv_obj_t* parent, main_tab_t active);

#endif // PODCAST_VIEW_TAB_BAR_H
