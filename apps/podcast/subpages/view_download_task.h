#ifndef PODCAST_VIEW_DOWNLOAD_TASK_H
#define PODCAST_VIEW_DOWNLOAD_TASK_H

#include <lvgl.h>

struct PodcastApp;

void podcast_view_download_task_init_registry(struct PodcastApp* app);

/* Tear down the singleton refresh timer + download-complete listener and clear
 * the active-page pointer.  Called from podcast_view_deinit. */
void podcast_view_download_task_deinit(void);

#endif // PODCAST_VIEW_DOWNLOAD_TASK_H
