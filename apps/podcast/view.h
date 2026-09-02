#ifndef PODCAST_VIEW_H
#define PODCAST_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"
#include "model.h"
#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PodcastApp;

#define PODCAST_PAGE_ID_MAX 11

enum podcast_page_id_t {
    PAGE_NONE = 0,
    PAGE_NETWORK,        // 1 — 网络页
    PAGE_LOCAL,          // 2 — 本地页
    PAGE_CHANNEL,          // 3 — 专辑页
    PAGE_PLAYER,         // 4 — 播放页
    PAGE_SEARCH,         // 5 — 搜索页
    PAGE_SEARCH_RESULTS, // 6 — 搜索结果页
    PAGE_PROFILE,        // 7 — 我的页
    PAGE_SETTINGS,       // 8 — 设置页
    PAGE_DOWNLOAD_TASK,  // 9 — 下载任务页
    PAGE_LOGIN,          // 10 — 登录页
};

typedef struct PodcastView {
    page_navigator_t page_nav;
} PodcastView;

void podcast_view_init(struct PodcastApp* app);
void podcast_view_deinit(struct PodcastApp* app);

// 子页面注册函数声明
void podcast_view_network_init_registry(struct PodcastApp* app);
void podcast_view_local_init_registry(struct PodcastApp* app);
void podcast_view_album_init_registry(struct PodcastApp* app);
void podcast_view_player_init_registry(struct PodcastApp* app);
void podcast_view_search_init_registry(struct PodcastApp* app);
void podcast_view_search_results_init_registry(struct PodcastApp* app);
void podcast_view_profile_init_registry(struct PodcastApp* app);
void podcast_view_settings_init_registry(struct PodcastApp* app);
void podcast_view_download_task_init_registry(struct PodcastApp* app);
void podcast_view_login_init_registry(struct PodcastApp* app);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PODCAST_VIEW_H
