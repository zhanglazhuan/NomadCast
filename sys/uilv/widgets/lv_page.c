/**
 * @file lv_page.c
 * @brief 标准页面骨架 — 状态栏占位 + 可选 Header + 内容区
 *
 * 状态栏是全局单例(位于 lv_layer_top)，页面内只保留一个透明占位
 * spacer 确保内容区不会被状态栏遮挡。
 */
#include <stdio.h>
#include <string.h>
#include "lv_page.h"
#include "esp_log.h"

static const char *TAG = "lv_page";

/* Global CJK font — declared in podcast/model.h, set by main.c */
extern const struct _lv_font_t *g_cjk_font;

Page lv_page_create(const char *title, bool allow_back, lv_event_cb_t back_cb, void *user_data)
{
    Page page;
    memset(&page, 0, sizeof(page));

    /* 1. 创建 screen */
    page.screen = lv_obj_create(NULL);
    lv_obj_set_size(page.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page.screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(page.screen, 0, 0);
    lv_obj_set_flex_flow(page.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(page.screen, LV_SCROLLBAR_MODE_OFF);
    /* Inherit CJK font on this screen — fallback chain handles LV_SYMBOL_* glyphs */
    if (g_cjk_font) lv_obj_set_style_text_font(page.screen, g_cjk_font, 0);

    /* 2. 状态栏占位 — 全局状态栏在 lv_layer_top，这里用透明 spacer 占 24px */
    {
        lv_obj_t *spacer = lv_obj_create(page.screen);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_size(spacer, LV_PCT(100), LV_STATUS_BAR_HEIGHT);
    }
    /* 获取全局状态栏引用（单例，所有页面共享） */
    page.status_bar = lv_status_bar_get();

    /* 3. Header (flex row: back | mid | right) */
    if (title == NULL && !allow_back) {
        page.header = NULL;
        page.header_right = NULL;
    } else {
        int header_h = LV_PAGE_HEADER_HEIGHT;

        page.header = lv_obj_create(page.screen);
        lv_obj_set_width(page.header, LV_PCT(100));
        lv_obj_set_height(page.header, header_h);
        lv_obj_set_scroll_dir(page.header, LV_DIR_NONE);
        lv_obj_set_style_border_width(page.header, 0, 0);
        lv_obj_set_style_pad_left(page.header, 8, 0);
        lv_obj_set_style_pad_right(page.header, 8, 0);
        lv_obj_set_style_pad_bottom(page.header, 0, 0);
        lv_obj_set_style_pad_top(page.header, 4, 0);
        lv_obj_set_style_bg_opa(page.header, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(page.header, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(page.header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(page.header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* 返回按钮 (或占位) */
        lv_obj_t *btn_back;
        if (allow_back) {
            btn_back = lv_button_create(page.header);
            lv_obj_set_style_border_width(btn_back, 0, 0);
            lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(btn_back, lv_color_black(), 0);
            lv_obj_set_style_shadow_width(btn_back, 0, 0);
            lv_obj_set_style_pad_all(btn_back, 0, 0);

            lv_obj_t *lbl = lv_label_create(btn_back);
            lv_label_set_text(lbl, LV_SYMBOL_LEFT);
            lv_obj_center(lbl);

            if (back_cb) {
                lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, user_data);
            }
        } else {
            btn_back = lv_obj_create(page.header);
            lv_obj_remove_style_all(btn_back);
        }
        lv_obj_set_width(btn_back, 24);
        lv_obj_set_height(btn_back, LV_PCT(100));

        /* 标题 > 弹性占位 */
        lv_obj_t *mid;
        if (title) {
            mid = lv_label_create(page.header);
            lv_label_set_text(mid, title);
            lv_obj_set_style_text_align(mid, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(mid, g_cjk_font ? g_cjk_font : &lv_font_montserrat_14, 0);
        } else {
            mid = lv_obj_create(page.header);
            lv_obj_remove_style_all(mid);
        }
        lv_obj_set_height(mid, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(mid, 1);

        /* 右槽位 */
        lv_obj_t *right_slot = lv_obj_create(page.header);
        lv_obj_remove_style_all(right_slot);
        lv_obj_set_width(right_slot, 24);
        lv_obj_set_height(right_slot, LV_PCT(100));
        page.header_right = right_slot;
    }

    /* 4. Container (内容区, 显式高度避免 flex-grow 循环依赖) */
    page.container = lv_obj_create(page.screen);
    lv_obj_set_width(page.container, LV_PCT(100));
    lv_obj_set_style_border_width(page.container, 0, 0);
    lv_obj_set_style_pad_all(page.container, 0, 0);
    lv_obj_set_flex_flow(page.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(page.container, LV_SCROLLBAR_MODE_OFF);
    {
        int h = lv_display_get_vertical_resolution(lv_display_get_default());
        h -= LV_STATUS_BAR_HEIGHT;  /* 24 — spacer */
        if (page.header) h -= LV_PAGE_HEADER_HEIGHT;  /* 28 */
        lv_obj_set_height(page.container, h);
    }

    ESP_LOGI(TAG, "create: \"%s\" (%sback)",
             title ? title : "(no title)", allow_back ? "" : "no ");
    return page;
}
