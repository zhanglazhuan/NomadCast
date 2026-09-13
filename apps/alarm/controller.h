#ifndef ALARM_CONTROLLER_H
#define ALARM_CONTROLLER_H

struct AlarmApp;
struct AlarmModel;
struct AlarmView;

typedef struct AlarmController {
    struct AlarmModel* model;
    struct AlarmView* view;
} AlarmController;

void alarm_controller_init(struct AlarmApp* app);
void alarm_controller_deinit(struct AlarmApp* app);

/* Navigation helpers — track the active page in the model. */
void alarm_nav_push(struct AlarmApp* app, int from, int to);
void alarm_nav_replace(struct AlarmApp* app, int to);

#endif // ALARM_CONTROLLER_H
