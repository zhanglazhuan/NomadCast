#ifndef ALARM_VIEW_H
#define ALARM_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AlarmApp;

typedef struct AlarmView {
    page_navigator_t page_nav;
} AlarmView;

#define ALARM_PAGE_ID_MAX 3

enum alarm_page_id_t {
    PAGE_NONE = 0,
    PAGE_LIST,   /* 1 — alarm list (root) */
    PAGE_EDIT,   /* 2 — add/edit a single alarm */
};

void alarm_view_init(struct AlarmApp* app);
void alarm_view_deinit(struct AlarmApp* app);

void alarm_view_list_init_registry(struct AlarmApp* app);
void alarm_view_edit_init_registry(struct AlarmApp* app);

#ifdef __cplusplus
}
#endif

#endif // ALARM_VIEW_H
