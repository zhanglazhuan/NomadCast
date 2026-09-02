/*
 * t_ui — UI Module Implementation
 *
 * LVGL 9.5 event callbacks for button click and left-swipe detection.
 */

#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "ui";

/* ---- Button click event callback ---- */

static void btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Button clicked!");
    }
}

/* ---- Swipe gesture handled at indev level (t_ui.c) — nothing here ---- */

/* ---- Public API ---- */

void ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();

    /* Centered button */
    lv_obj_t *btn = lv_button_create(screen);
    lv_obj_set_size(btn, 140, 50);
    lv_obj_center(btn);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    /* Button label */
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "Click Me");
    lv_obj_center(label);

    ESP_LOGI(TAG, "UI initialized: button");
}
