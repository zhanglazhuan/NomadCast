#ifndef ALARM_MODEL_H
#define ALARM_MODEL_H

struct AlarmApp;

/* The alarm data itself lives in the global alarm_service (single source of
 * truth). This model only tracks per-app UI state. */
typedef struct AlarmModel {
    int current_page;   /* PAGE_LIST / PAGE_EDIT (view.h) */
    int editing_index;  /* alarm being edited, -1 = adding a new one */
} AlarmModel;

void alarm_model_init(struct AlarmApp* app);
void alarm_model_deinit(struct AlarmApp* app);

#endif // ALARM_MODEL_H
