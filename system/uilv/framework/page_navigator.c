#include <stdio.h>
#include <stdlib.h>
#include "page_navigator.h"
#include "esp_log.h"

static const char *TAG = "nav";

static page_builder_cb_t find_builder(page_navigator_t *nav, int page_id)
{
	for (int i = 0; i < nav->registry_count; i++) {
		if (nav->registry[i].page_id == page_id) {
			return nav->registry[i].builder;
		}
	}
	return NULL;
}

void page_navigator_init(page_navigator_t *nav, page_navigator_page_t *registry,
			 int max_pages, app_handle_t app)
{
	nav->registry = registry;
	nav->registry_size = max_pages;
	nav->registry_count = 0;
	nav->stack_top = -1;
	nav->nav_ctx = NULL;
	nav->app_handler = app;
}

int page_navigator_register_page(page_navigator_t *nav, int page_id, page_builder_cb_t builder)
{
	if (nav->registry_count >= nav->registry_size) {
		ESP_LOGE(TAG, "registry full");
		return -1;
	}
	nav->registry[nav->registry_count].page_id = page_id;
	nav->registry[nav->registry_count].builder = builder;
	nav->registry_count++;
	return 0;
}

void page_navigator_navigate_to(page_navigator_t *nav, app_handle_t app, int page_id, void *user_data)
{
	page_builder_cb_t builder = find_builder(nav, page_id);
	if (!builder) {
		ESP_LOGE(TAG, "no builder for page %d", page_id);
		return;
	}

	/* 旧上下文由 builder 自行释放 (在其 build 函数开头 free)，
	 * 或由旧 screen 的 LV_EVENT_DELETE 回调释放。
	 * 不在此处 free，避免与 delete 回调冲突导致双重释放。 */
	nav->nav_ctx = user_data;

	/* Build the new screen while the old one is still active so act_scr
	 * is never dangling.  After loading the new screen, use async delete
	 * for the old screen — lv_obj_delete_async defers the actual deletion
	 * to a later timer callback, outside the current event handler.
	 * Calling lv_obj_delete synchronously from within an LVGL event
	 * handler (e.g. a button click on the old screen) corrupts internal
	 * LVGL state because the event source is freed before the event
	 * dispatch completes. */
	lv_obj_t *old_screen = lv_screen_active();
	lv_obj_t *new_screen = builder(app, user_data);

	if (new_screen) {
		lv_screen_load(new_screen);
		if (old_screen) lv_obj_delete_async(old_screen);
		ESP_LOGI(TAG, "navigated to page %d", page_id);
	} else {
		ESP_LOGE(TAG, "builder returned NULL for page %d", page_id);
	}
}

bool page_navigator_navigate_pop(page_navigator_t *nav, app_handle_t app)
{
	if (!nav) return false;

	if (nav->stack_top < 0) {
		ESP_LOGI(TAG, "already at root, cannot go back");
		return false;
	}

	int prev_id = nav->stack[nav->stack_top];
	nav->stack_top--;

	/* 旧上下文由旧 screen 的 LV_EVENT_DELETE 回调释放 */
	nav->nav_ctx = NULL;

	page_builder_cb_t builder = find_builder(nav, prev_id);
	if (!builder) {
		ESP_LOGE(TAG, "no builder for page %d", prev_id);
		return false;
	}

	/* Build new screen before deleting the old one, and use async
	 * delete — same rationale as page_navigator_navigate_to. */
	lv_obj_t *old_screen = lv_screen_active();
	lv_obj_t *new_screen = builder(app, NULL);
	if (new_screen) {
		lv_screen_load(new_screen);
		if (old_screen) lv_obj_delete_async(old_screen);
		ESP_LOGI(TAG, "popped to page %d", prev_id);
		return true;
	}
	return false;
}

void page_navigator_push(page_navigator_t *nav, int page_id)
{
	if (!nav || page_id <= 0) return;
	if (nav->stack_top >= 9) return;
	nav->stack[++nav->stack_top] = page_id;
}

void page_navigator_navigate_back(lv_event_t *e)
{
	page_navigator_t *nav = (page_navigator_t *)lv_event_get_user_data(e);
	if (nav) {
		page_navigator_navigate_pop(nav, nav->app_handler);
	}
}

void page_navigator_deinit(page_navigator_t *nav)
{
	if (!nav) return;
	nav->registry = NULL;
	nav->registry_size = 0;
	nav->registry_count = 0;
	nav->stack_top = -1;
	nav->app_handler = NULL;
	if (nav->nav_ctx) {
		lv_free(nav->nav_ctx);
		nav->nav_ctx = NULL;
	}
}
