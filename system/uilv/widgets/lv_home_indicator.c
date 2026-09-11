#include "lv_home_indicator.h"

lv_obj_t *lv_home_indicator_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);

    lv_obj_set_size(bar, 80, 3);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);   /* semicircle ends */

    /* FLOATING escapes the parent's flex-column so it pins to the screen edge. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);    /* let taps/swipes pass through */
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    return bar;
}
