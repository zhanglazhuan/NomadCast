#ifndef PODCAST_VIEW_NETWORK_H
#define PODCAST_VIEW_NETWORK_H

#include <lvgl.h>

struct PodcastApp;

void podcast_view_network_init_registry(struct PodcastApp* app);

/* Unregister the artwork-ready listener and clear the active-page pointer.
 * Called from podcast_view_deinit so a later queued artwork event can't touch
 * a torn-down page. */
void podcast_view_network_deinit(void);

#endif // PODCAST_VIEW_NETWORK_H
