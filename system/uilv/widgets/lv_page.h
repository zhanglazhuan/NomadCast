#ifndef LV_PAGE_WIDGET_H
#define LV_PAGE_WIDGET_H

#include <lvgl.h>
#include "lv_status_bar.h"

/** Header 高度 — 修改此值即可全局调整 */
#define LV_PAGE_HEADER_HEIGHT 28

/**
 * @brief 标准页面骨架
 *
 * 使用 lv_page_create() 创建标准页面，包含状态栏（顶部）、header（返回按钮 + 标题 +
 * 右槽位）和 container（内容区）。所有自定义内容应添加到 container 中。
 *
 * 用法:
 *   Page page = lv_page_create("Title", true, navigate_back_cb, &nav);
 *   // 构建内容到 page.container 中
 *   lv_obj_t *btn = lv_button_create(page.container);
 *   return page.screen;
 */
typedef struct {
    lv_obj_t *screen;           // 根 screen 对象
    lv_status_bar_t *status_bar; // 全局状态栏 (单例，跨页面共享)
    lv_obj_t *header;           // 顶部栏 (flex row: 返回按钮 | 标题 | 右槽位)
    lv_obj_t *header_right;     // 右槽位 (可放入自定义图标按钮)
    lv_obj_t *container;        // 内容容器 (flex column, 显式高度)
} Page;

/**
 * @brief 创建标准页面
 * @param title      标题文字 (NULL = 无标题)
 * @param allow_back 是否显示返回按钮
 * @param back_cb    返回按钮回调 (LV_EVENT_CLICKED)
 * @param user_data  回调用户数据 (通常是 &page_navigator)
 * @return Page 结构体
 */
Page lv_page_create(const char *title, bool allow_back, lv_event_cb_t back_cb, void *user_data);

#endif
