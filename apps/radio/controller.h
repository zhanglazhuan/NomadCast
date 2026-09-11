#ifndef RADIO_CONTROLLER_H
#define RADIO_CONTROLLER_H

struct RadioApp;
struct RadioModel;
struct RadioView;

typedef struct RadioController {
    struct RadioModel* model;
    struct RadioView* view;
} RadioController;

void radio_controller_init(struct RadioApp* app);
void radio_controller_deinit(struct RadioApp* app);

/* Play a station by index. */
void radio_controller_play(struct RadioApp* app, int idx);

/* Toggle play/pause for the current stream. */
void radio_controller_toggle(struct RadioApp* app);

/* Change to the next / previous station (wrap-around). */
void radio_controller_next(struct RadioApp* app);
void radio_controller_prev(struct RadioApp* app);

/* Navigation helpers — track the active page in the model so the physical
 * next/prev keys can decide between "switch station in place" and "open the
 * playing page". */
void radio_nav_push(struct RadioApp* app, int from, int to);
void radio_nav_replace(struct RadioApp* app, int to);

#endif // RADIO_CONTROLLER_H
