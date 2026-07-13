/**
 * @file lv_bottom_sheet.c
 * @brief Bottom sheet widget (lv_malloc/lv_free)
 */
#include <stdio.h>
#include <string.h>
#include "lv_bottom_sheet.h"

static void delete_event_cb(lv_event_t * e) {
    lv_bottom_sheet_t * bs = lv_event_get_user_data(e);
    if (bs) { lv_free(bs); }
}

static void close_btn_event_cb(lv_event_t * e) {
    lv_bottom_sheet_close(lv_event_get_user_data(e));
}

static void overlay_event_cb(lv_event_t * e) {
    lv_obj_t * target = lv_event_get_target(e);
    if (target == lv_event_get_current_target(e)) {
        lv_obj_delete_async(target);
    }
}

lv_bottom_sheet_t * lv_bottom_sheet_create(lv_obj_t * parent) {
    lv_bottom_sheet_t * bs = lv_malloc(sizeof(lv_bottom_sheet_t));
    if (!bs) return NULL;
    memset(bs, 0, sizeof(lv_bottom_sheet_t));

    if (!parent) parent = lv_screen_active();

    // Overlay
    bs->overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(bs->overlay);
    lv_obj_set_size(bs->overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(bs->overlay, 192, 0);  /* 75% opacity */
    lv_obj_set_style_bg_color(bs->overlay, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(bs->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bs->overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(bs->overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_foreground(bs->overlay);
    lv_obj_add_event_cb(bs->overlay, overlay_event_cb, LV_EVENT_CLICKED, bs);
    lv_obj_add_event_cb(bs->overlay, delete_event_cb, LV_EVENT_DELETE, bs);

    // Sheet panel
    bs->sheet = lv_obj_create(bs->overlay);
    lv_obj_remove_style_all(bs->sheet);
    lv_obj_set_width(bs->sheet, LV_PCT(100));
    lv_obj_set_height(bs->sheet, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bs->sheet, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bs->sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bs->sheet, 0, 0);
    lv_obj_align(bs->sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(bs->sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(bs->sheet, LV_OBJ_FLAG_CLICKABLE);

    // Content
    bs->content = lv_obj_create(bs->sheet);
    lv_obj_remove_style_all(bs->content);
    lv_obj_set_width(bs->content, LV_PCT(100));
    lv_obj_set_height(bs->content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bs->content, 16, 0);
    lv_obj_clear_flag(bs->content, LV_OBJ_FLAG_CLICKABLE);

    return bs;
}

lv_obj_t * lv_bottom_sheet_add_header(lv_bottom_sheet_t * bs, const char * title) {
    if (!bs || !bs->sheet) return NULL;
    bs->header = lv_obj_create(bs->sheet);
    lv_obj_remove_style_all(bs->header);
    lv_obj_set_width(bs->header, LV_PCT(100));
    lv_obj_set_height(bs->header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bs->header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bs->header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bs->header, 16, 0);
    lv_obj_set_style_border_width(bs->header, 2, 0);
    lv_obj_set_style_border_side(bs->header, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);

    if (title) {
        lv_obj_t * tl = lv_label_create(bs->header);
        lv_label_set_text(tl, title);
    } else {
        lv_obj_t * ph = lv_obj_create(bs->header);
        lv_obj_remove_style_all(ph);
    }

    lv_obj_t * cb = lv_button_create(bs->header);
    lv_obj_set_size(cb, 40, 40);
    lv_obj_set_style_bg_opa(cb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(cb, 0, 0);
    lv_obj_t * cl = lv_label_create(cb);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cb, close_btn_event_cb, LV_EVENT_CLICKED, bs);
    lv_obj_move_to_index(bs->header, 0);
    return bs->header;
}

lv_obj_t * lv_bottom_sheet_get_content(lv_bottom_sheet_t * bs) {
    return bs ? bs->content : NULL;
}

void lv_bottom_sheet_set_height(lv_bottom_sheet_t * bs, lv_coord_t height) {
    if (bs && bs->sheet) lv_obj_set_height(bs->sheet, height);
}

void lv_bottom_sheet_close(lv_bottom_sheet_t * bs) {
    if (bs && bs->overlay) lv_obj_delete_async(bs->overlay);
}
