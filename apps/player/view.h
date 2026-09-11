#ifndef PLAYER_VIEW_H
#define PLAYER_VIEW_H

#include <lvgl.h>
#include "page_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PlayerApp;

typedef struct PlayerView {
    page_navigator_t page_nav;
} PlayerView;

#define PLAYER_PAGE_ID_MAX 3

enum player_page_id_t {
    PAGE_NONE = 0,
    PAGE_FILES,   /* 1 — SD 文件浏览器（根 + 子目录复用此页，就地重建） */
    PAGE_PLAYER,  /* 2 — 播放页（now-playing） */
};

void player_view_init(struct PlayerApp* app);
void player_view_deinit(struct PlayerApp* app);

void player_view_files_init_registry(struct PlayerApp* app);
void player_view_player_init_registry(struct PlayerApp* app);

#ifdef __cplusplus
}
#endif

#endif // PLAYER_VIEW_H
