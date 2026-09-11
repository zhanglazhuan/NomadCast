#include <stdlib.h>
#include <string.h>
#include "controller.h"
#include "app.h"
#include "model.h"
#include "view.h"
#include "audio_player.h"
#include "input.h"
#include "lv_toast.h"
#include "lang.h"
#include "esp_log.h"

static const char *TAG = "radio_ctrl";

/* ── UI-thread entry points (posted from the button task via lv_async_call) ── */

static void async_next(void *ud)   { radio_controller_next((RadioApp*)ud); }
static void async_prev(void *ud)   { radio_controller_prev((RadioApp*)ud); }
static void async_toggle(void *ud) { radio_controller_toggle((RadioApp*)ud); }

/* Physical key callback — runs on the button task.  Touching LVGL objects or
 * calling audio_player_play() directly here is unsafe, so re-dispatch to the
 * LVGL task via lv_async_call (drained by lv_timer_handler in the main loop). */
static void on_radio_input(input_event_t event, void *user_data)
{
    switch (event) {
    case INPUT_EVENT_NEXT_TRACK: lv_async_call(async_next, user_data);   break;
    case INPUT_EVENT_PREV_TRACK: lv_async_call(async_prev, user_data);   break;
    case INPUT_EVENT_PLAY_PAUSE: lv_async_call(async_toggle, user_data); break;
    default: break;
    }
}

void radio_controller_init(struct RadioApp* app) {
    app->controller = (RadioController*)malloc(sizeof(RadioController));
    if (!app->controller) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }
    app->controller->model = NULL;
    app->controller->view  = NULL;
    input_subscribe(on_radio_input, app);
    ESP_LOGI(TAG, "init done");
}

void radio_controller_deinit(struct RadioApp* app) {
    /* Stop any live stream and free the whole ADF pipeline before the app is
     * torn down, so exiting to the launcher kills playback instead of leaving
     * the radio playing in the background. */
    if (audio_player_is_active()) audio_player_stop();

    if (app->controller) {
        input_unsubscribe(on_radio_input);
        free(app->controller);
        app->controller = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}

void radio_controller_play(struct RadioApp* app, int idx) {
    if (!app || !app->model) return;
    if (idx < 0 || idx >= app->model->count) return;

    app->model->current = idx;
    audio_player_init();
    bool ok = audio_player_play(app->model->stations[idx].url);
    if (!ok) {
        lv_toast_show(tr(STR_RADIO_FAILED_TO_PLAY), 2000);
    }
}

void radio_controller_toggle(struct RadioApp* app) {
    (void)app;
    if (audio_player_is_playing()) {
        audio_player_pause(true);
    } else if (audio_player_is_active()) {
        audio_player_pause(false);
    }
}

void radio_controller_next(struct RadioApp* app) {
    if (!app || !app->model || app->model->count == 0) return;
    int idx = app->model->current;
    if (idx < 0) idx = 0;
    else         idx = (idx + 1) % app->model->count;
    radio_controller_play(app, idx);
    if (app->model->current_page != PAGE_PLAYING) {
        radio_nav_replace(app, PAGE_PLAYING);
    }
}

void radio_controller_prev(struct RadioApp* app) {
    if (!app || !app->model || app->model->count == 0) return;
    int idx = app->model->current;
    if (idx < 0) idx = 0;
    else         idx = (idx - 1 + app->model->count) % app->model->count;
    radio_controller_play(app, idx);
    if (app->model->current_page != PAGE_PLAYING) {
        radio_nav_replace(app, PAGE_PLAYING);
    }
}

void radio_nav_push(struct RadioApp* app, int from, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    PAGE_NAVIGATE_TO(app, from, to, NULL);
}

void radio_nav_replace(struct RadioApp* app, int to) {
    if (!app || !app->model) return;
    app->model->current_page = to;
    page_navigator_navigate_to(&app->view->page_nav, app, to, NULL);
}
