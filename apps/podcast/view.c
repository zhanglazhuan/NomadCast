#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"

// 子页面头文件
#include "subpages/view_network.h"
#include "subpages/view_local.h"
#include "subpages/view_channel.h"
#include "subpages/view_player.h"
#include "subpages/view_search.h"
#include "subpages/view_search_results.h"
#include "subpages/view_profile.h"
#include "subpages/view_settings.h"
#include "subpages/view_download_task.h"
#include "subpages/view_login.h"

// 全局应用实例
extern PodcastApp g_podcast_app;

void podcast_view_init(struct PodcastApp* app) {
    app->view = (PodcastView*)malloc(sizeof(PodcastView));
    if (!app->view) {
        printf("[ERR] View memory allocation failed\n");
        return;
    }

    page_navigator_page_t *page_builders = (page_navigator_page_t*)malloc(sizeof(page_navigator_page_t) * PODCAST_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * PODCAST_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, PODCAST_PAGE_ID_MAX, app);

    // 注册子页面
    podcast_view_network_init_registry(app);
    podcast_view_local_init_registry(app);
    podcast_view_album_init_registry(app);
    podcast_view_player_init_registry(app);
    podcast_view_search_init_registry(app);
    podcast_view_search_results_init_registry(app);
    podcast_view_profile_init_registry(app);
    podcast_view_settings_init_registry(app);
    podcast_view_download_task_init_registry(app);
    podcast_view_login_init_registry(app);

    printf("[INF] podcast_view_init done (10 subpages registered)\n");
}

void podcast_view_deinit(struct PodcastApp* app) {
    if (app->view) {
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    printf("[INF] podcast_view_deinit done\n");
}
