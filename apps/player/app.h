#ifndef PLAYER_APP_H
#define PLAYER_APP_H

#include <lvgl.h>

struct PlayerModel;
struct PlayerView;
struct PlayerController;

typedef struct PlayerApp {
    struct PlayerModel* model;
    struct PlayerView* view;
    struct PlayerController* controller;
} PlayerApp;

extern PlayerApp g_player_app;

void player_app_register(void);  /* self-register with app_manager */

#endif // PLAYER_APP_H
