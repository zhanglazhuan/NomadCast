#ifndef PODCAST_VIEW_PLAYER_H
#define PODCAST_VIEW_PLAYER_H

#include <lvgl.h>

struct PodcastApp;

void podcast_view_player_init_registry(struct PodcastApp* app);

/* Argument passed to PAGE_PLAYER navigation. Exactly one field is used:
 *   episode_id > 0 → play this episode (channel → player flow)
 *   path[0] != 0   → play this local file directly (recently-played flow)
 * NULL means "show the currently-loaded episode" (tab bar flow). */
typedef struct {
    int   episode_id;
    char  path[2560];
} player_open_arg_t;

#endif // PODCAST_VIEW_PLAYER_H
