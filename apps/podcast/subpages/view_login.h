#ifndef PODCAST_VIEW_LOGIN_H
#define PODCAST_VIEW_LOGIN_H

#include <lvgl.h>

struct PodcastApp;

typedef enum {
    LOGIN_MODE_LOGIN = 0,
    LOGIN_MODE_REGISTER = 1,
} login_mode_t;

void podcast_view_login_init_registry(struct PodcastApp* app);

#endif // PODCAST_VIEW_LOGIN_H
