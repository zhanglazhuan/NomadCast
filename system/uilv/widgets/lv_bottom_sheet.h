#ifndef LV_BOTTOM_SHEET_H
#define LV_BOTTOM_SHEET_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * sheet;
    lv_obj_t * header;
    lv_obj_t * content;
} lv_bottom_sheet_t;

lv_bottom_sheet_t * lv_bottom_sheet_create(lv_obj_t * parent);
lv_obj_t * lv_bottom_sheet_add_header(lv_bottom_sheet_t * bs, const char * title);
lv_obj_t * lv_bottom_sheet_get_content(lv_bottom_sheet_t * bs);
void lv_bottom_sheet_set_height(lv_bottom_sheet_t * bs, lv_coord_t height);
void lv_bottom_sheet_close(lv_bottom_sheet_t * bs);

#ifdef __cplusplus
}
#endif

#endif
