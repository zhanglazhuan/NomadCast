/*
 * NomadCast — Global Gesture Detection
 *
 * Gestures (adjusted for 240×320 ST7789):
 *   Swipe up from bottom → "Exit App" confirm dialog
 */

#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"
#include "app_manager.h"
#include "launcher.h"
#include "lv_bottom_sheet.h"

static const char *TAG = "gesture";

/* ---- Thresholds (adjusted for 240×320) ---- */

#define SWIPE_UP_DY_THRESHOLD   -15
#define SWIPE_UP_START_Y_MIN     300

/* ---- Exit confirm bottom sheet ---- */

static lv_bottom_sheet_t *g_exit_sheet = NULL;

static void exit_sheet_on_delete(lv_event_t *e)
{
    (void)e;
    g_exit_sheet = NULL;
}

static void exit_yes_cb(lv_event_t *e)
{
    (void)e;
    if (g_exit_sheet) { lv_bottom_sheet_close(g_exit_sheet); g_exit_sheet = NULL; }
    launcher_return_home();
}

static void exit_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (g_exit_sheet) { lv_bottom_sheet_close(g_exit_sheet); g_exit_sheet = NULL; }
}

static void show_exit_confirm_dialog(void)
{
    if (g_exit_sheet) return;

    g_exit_sheet = lv_bottom_sheet_create(lv_layer_top());
    lv_obj_add_event_cb(g_exit_sheet->overlay, exit_sheet_on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *cont = lv_bottom_sheet_get_content(g_exit_sheet);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    const char *app_name = app_manager_get_current_app_name();
    lv_obj_t *msg = lv_label_create(cont);
    lv_label_set_text_fmt(msg, "Exit \"%s\" and\nreturn to launcher?", app_name ? app_name : "App");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x666666), 0);
    lv_obj_set_width(msg, LV_PCT(100));

    lv_obj_t *btn_row = lv_obj_create(cont);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(btn_row, 8, 0);

    lv_obj_t *cancel = lv_button_create(btn_row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_t *ca_lb = lv_label_create(cancel);
    lv_label_set_text(ca_lb, "Cancel");
    lv_obj_center(ca_lb);
    lv_obj_set_style_text_font(ca_lb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ca_lb, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(cancel, exit_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exit_btn = lv_button_create(btn_row);
    lv_obj_set_flex_grow(exit_btn, 1);
    lv_obj_set_style_bg_color(exit_btn, lv_color_hex(0x1976D2), 0);
    lv_obj_t *ex_lb = lv_label_create(exit_btn);
    lv_label_set_text(ex_lb, "Exit");
    lv_obj_set_style_text_color(ex_lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ex_lb);
    lv_obj_set_style_text_font(ex_lb, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(exit_btn, exit_yes_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- Global indev monitor ---- */

static void global_indev_event_cb(lv_event_t *e)
{
    static lv_point_t start_pt;

    lv_indev_t *indev = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &start_pt);
    } else if (code == LV_EVENT_RELEASED) {
        lv_point_t end_pt;
        lv_indev_get_point(indev, &end_pt);

        int32_t dy = end_pt.y - start_pt.y;
        /* ESP_LOGI(TAG, "Release: (%d,%d)→(%d,%d) dy=%ld", start_pt.x, start_pt.y, end_pt.x, end_pt.y, (long)dy); */

        /* Swipe UP from bottom → exit confirm (only when an app is running) */
        if (dy < SWIPE_UP_DY_THRESHOLD && start_pt.y > SWIPE_UP_START_Y_MIN) {
            if (app_manager_get_current_app_name() == NULL) {
                /* Already on launcher — nothing to exit to */
                return;
            }
            ESP_LOGI(TAG, "→ Swipe-up → exit confirm");
            show_exit_confirm_dialog();
            return;
        }
    }
}

/* ---- Public API ---- */

void launcher_gesture_init(lv_indev_t *touch_indev)
{
    if (touch_indev) {
        lv_indev_add_event_cb(touch_indev, global_indev_event_cb, LV_EVENT_ALL, NULL);
        ESP_LOGI(TAG, "Gesture hooks installed on touch indev");
    } else {
        ESP_LOGE(TAG, "Cannot hook gestures: touch_indev is NULL");
    }
}
