#ifndef PLAYER_CONTROLLER_H
#define PLAYER_CONTROLLER_H

struct PlayerApp;
struct PlayerModel;
struct PlayerView;

typedef struct PlayerController {
    struct PlayerModel* model;
    struct PlayerView* view;
} PlayerController;

void player_controller_init(struct PlayerApp* app);
void player_controller_deinit(struct PlayerApp* app);

/* Start playing a local SD audio file. */
void player_controller_play_file(struct PlayerApp* app, const char* path);

/* Navigation helpers — track the active page in the model so the physical
 * back key can decide between "up one folder" and "pop the page stack". */
void player_nav_push(struct PlayerApp* app, int from, int to);
void player_nav_replace(struct PlayerApp* app, int to);
void player_go_up(struct PlayerApp* app);

#endif // PLAYER_CONTROLLER_H
