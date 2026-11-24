#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

#include "view.h"
#include "app.h"
#include "model.h"
#include "subpages/view_browse.h"
#include "subpages/view_favorites.h"
#include "subpages/view_stations.h"
#include "subpages/view_playing.h"
#include "app_event.h"
#include "launcher.h"
#include "app_manager.h"
#include "lang.h"
#include "esp_log.h"

static const char *TAG = "radio_view";

/* Global CJK font — set by main.c. */
extern const lv_font_t *g_cjk_font;

/* ── Offline hint: "无网络" + "打开设置" (mirrors Podcast view_network) ── */

static void radio_deferred_open_settings(lv_timer_t *timer) {
    launcher_open_app("Settings");
    lv_timer_del(timer);
}

static void radio_on_open_settings(lv_event_t *e) {
    (void)e;
    /* Close the radio app, then open Settings (deferred to avoid use-after-free). */
    app_manager_delete();
    lv_timer_create(radio_deferred_open_settings, 300, NULL);
}

void radio_view_wifi_offline(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, tr(STR_NO_NETWORK));
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label, g_cjk_font, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(label, LV_PCT(100), LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn, 160, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, radio_on_open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 24);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, tr(STR_OPEN_SETTINGS));
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(btn_label, g_cjk_font, 0);
    lv_obj_center(btn_label);
}

/* Worker task completes a network op → APP_EVENT_RADIO_UPDATED. Rebuild the
 * active page from the model. Region/station/favorite pages all render purely
 * from the model, so re-running the builder is safe. */
static void on_radio_event(app_event_t event, const void *data)
{
    (void)data;
    if (event != APP_EVENT_RADIO_UPDATED) return;

    RadioApp *app = &g_radio_app;
    if (!app || !app->view || !app->model) return;

    int pg = app->model->current_page;
    if (pg == PAGE_NONE) pg = PAGE_BROWSE;
    page_navigator_navigate_to(&app->view->page_nav, app, pg, NULL);
}

void radio_view_init(struct RadioApp* app) {
    app->view = (RadioView*)malloc(sizeof(RadioView));
    if (!app->view) {
        ESP_LOGE(TAG, "memory allocation failed");
        return;
    }

    page_navigator_page_t* page_builders = (page_navigator_page_t*)malloc(
        sizeof(page_navigator_page_t) * RADIO_PAGE_ID_MAX);
    memset(page_builders, 0, sizeof(page_navigator_page_t) * RADIO_PAGE_ID_MAX);
    page_navigator_init(&app->view->page_nav, page_builders, RADIO_PAGE_ID_MAX, app);

    radio_view_browse_init_registry(app);
    radio_view_favorites_init_registry(app);
    radio_view_stations_init_registry(app);
    radio_view_playing_init_registry(app);

    app_event_register(on_radio_event);

    ESP_LOGI(TAG, "init done (4 pages registered)");
}

void radio_view_deinit(struct RadioApp* app) {
    if (app->view) {
        app_event_unregister(on_radio_event);
        page_navigator_deinit(&app->view->page_nav);
        free(app->view->page_nav.registry);
        free(app->view);
        app->view = NULL;
    }
    ESP_LOGI(TAG, "deinit done");
}
