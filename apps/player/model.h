#ifndef PLAYER_MODEL_H
#define PLAYER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

struct PlayerApp;

#define PLAYER_SD_ROOT  "/sdcard"
#define PLAYER_PATH_MAX 256

typedef struct PlayerModel {
    char current_file[PLAYER_PATH_MAX];  /* last played file (full path) */
    char current_dir[PLAYER_PATH_MAX];   /* directory shown by the Files page */
    int  current_page;                    /* PAGE_FILES / PAGE_PLAYER (view.h) */
} PlayerModel;

void player_model_init(struct PlayerApp* app);
void player_model_deinit(struct PlayerApp* app);

#endif // PLAYER_MODEL_H
