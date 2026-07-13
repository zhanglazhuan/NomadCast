#ifndef PODCAST_APP_H
#define PODCAST_APP_H

#include <lvgl.h>

// 前向声明
struct PodcastView;
struct PodcastModel;
struct PodcastController;

typedef struct PodcastApp {
    struct PodcastModel* model;
    struct PodcastView* view;
    struct PodcastController* controller;
} PodcastApp;

extern PodcastApp g_podcast_app;

void podcast_app_init(void);
void podcast_app_deinit(void);
void podcast_app_register(void);
void podcast_app_startup(void);

#endif // PODCAST_APP_H
