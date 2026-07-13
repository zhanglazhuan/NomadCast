#ifndef PAGE_NAVIGATOR_H
#define PAGE_NAVIGATOR_H

#include <lvgl.h>

typedef void *app_handle_t;

typedef lv_obj_t *(*page_builder_cb_t)(app_handle_t app, void *user_data);

typedef struct {
	int page_id;
	page_builder_cb_t builder;
} page_navigator_page_t;

typedef struct {
	page_navigator_page_t *registry;
	int registry_size;
	int registry_count;
	int stack[10];
	int stack_top;
	void *nav_ctx;
	app_handle_t app_handler;  // 用于 navigate_back 等回调
} page_navigator_t;

// 压入返回栈 (由 PAGE_NAVIGATE_TO 宏自动调用)
void page_navigator_push(page_navigator_t *nav, int page_id);

// 初始化
void page_navigator_init(page_navigator_t *nav, page_navigator_page_t *registry, int max_pages, app_handle_t app);

// 页面注册
int page_navigator_register_page(page_navigator_t *nav, int page_id, page_builder_cb_t builder);

// 导航
void page_navigator_navigate_to(page_navigator_t *nav, app_handle_t app, int page_id, void *user_data);

// 返回上一页 (returns true if popped, false if at root)
bool page_navigator_navigate_pop(page_navigator_t *nav, app_handle_t app);

// LVGL 事件回调 — 用于绑定返回按钮
void page_navigator_navigate_back(lv_event_t *e);

// 清理
void page_navigator_deinit(page_navigator_t *nav);

// ---- 宏 ----

// 跳转到指定页面 (from 用于返回栈)
// 用法: PAGE_NAVIGATE_TO(&g_xxx_app, FROM_PAGE, TO_PAGE, user_data)
#define PAGE_NAVIGATE_TO(app_ptr, from, to, data) \
	do { \
		page_navigator_push(&(app_ptr)->view->page_nav, (from)); \
		page_navigator_navigate_to(&(app_ptr)->view->page_nav, (app_ptr), (to), (data)); \
	} while(0)

// 注册子页面 builder
// 用法: PAGE_REGISTE(app, PAGE_ID, build_func)
#define PAGE_REGISTE(app, page_id, builder_func) \
	page_navigator_register_page(&(app)->view->page_nav, (page_id), (page_builder_cb_t)(builder_func))

#endif
