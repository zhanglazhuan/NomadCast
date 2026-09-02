/*
 * NomadCast — Home UI (App Icon Grid)
 *
 * Adapted from EPOS/Zephyr (epos_home_ui.c) to ESP-IDF.
 *
 * Shows a 3-column grid of registered (non-hidden) apps.
 * Click any icon to launch the app via launcher_open_app().
 */

#include "esp_log.h"
#include "lvgl.h"
#include "app_manager.h"
#include "lv_page.h"
#include "launcher.h"

static const char *TAG = "home_ui";

/* ---- App icon click → launch ---- */

static void app_launcher_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    application_t *app = (application_t *)lv_event_get_user_data(e);
    if (!app || !app->name) return;

    /* Highlight selected icon with a thicker border */
    lv_obj_t *app_btn = lv_event_get_target(e);
    lv_obj_t *icon_cont = lv_obj_get_child(app_btn, 0);
    if (icon_cont) {
        lv_obj_set_style_border_width(icon_cont, 3, LV_PART_MAIN);
    }

    ESP_LOGI(TAG, "Launching: %s", app->name);
    launcher_open_app(app->name);
}

/* ---- Grid layout ---- */

static void create_grid_container(lv_obj_t *container,
                                  lv_coord_t *out_cell_w,
                                  lv_coord_t *out_cell_h,
                                  lv_coord_t *out_gap)
{
    lv_coord_t screen_w = lv_disp_get_hor_res(NULL);
    lv_coord_t pad_all     = 10;
    lv_coord_t pad_column  = 20;
    lv_coord_t pad_row     = 20;
    lv_coord_t gap         = 8;

    lv_coord_t avail = screen_w - (pad_all * 2) - (pad_column * 2) - 15; /* 15 = scrollbar reserve */
    lv_coord_t cell_w = avail / 2;
    lv_coord_t font_h = lv_font_get_line_height(&lv_font_montserrat_16);
    lv_coord_t cell_h = 80 + font_h + gap;

    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[11];
    for (int i = 0; i < 10; i++) row_dsc[i] = cell_h;
    row_dsc[10] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_layout(container, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(container, pad_all, LV_PART_MAIN);
    lv_obj_set_style_pad_row(container, pad_row, LV_PART_MAIN);
    lv_obj_set_style_pad_column(container, pad_column, LV_PART_MAIN);
    lv_obj_set_grid_dsc_array(container, col_dsc, row_dsc);

    if (out_cell_w) *out_cell_w = cell_w;
    if (out_cell_h) *out_cell_h = cell_h;
    if (out_gap)    *out_gap    = gap;
}

/* ---- Single app grid button ---- */

static lv_obj_t *create_app_grid_button(lv_obj_t *container, application_t *app,
                                         int col, int row,
                                         lv_coord_t cell_w, lv_coord_t gap)
{
    lv_obj_t *btn = lv_button_create(container);
    lv_obj_set_size(btn, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(btn, gap, LV_PART_MAIN);

    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                              LV_GRID_ALIGN_STRETCH, row, 1);

    lv_obj_add_event_cb(btn, app_launcher_event_cb, LV_EVENT_CLICKED, app);

    /* Icon container (square, bordered) */
    lv_obj_t *icon_cont = lv_obj_create(btn);
    lv_obj_set_size(icon_cont, 80, 80);
    lv_obj_set_style_bg_opa(icon_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_cont, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(icon_cont, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(icon_cont, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_CLICKABLE);

    /* App icon or fallback */
    if (app->icon) {
        lv_obj_t *img = lv_img_create(icon_cont);
        lv_img_set_src(img, app->icon);
        lv_obj_center(img);
    } else {
        lv_obj_t *fb = lv_label_create(icon_cont);
        lv_label_set_text(fb, LV_SYMBOL_DUMMY);
        lv_obj_set_style_text_font(fb, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(fb, lv_color_black(), LV_PART_MAIN);
        lv_obj_center(fb);
    }

    /* App name label */
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, app->name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(label, cell_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    return btn;
}

/* ---- Public API ---- */

void launcher_home_ui(void)
{
    Page home_page = lv_page_create("Applications", false, NULL, NULL);
    lv_obj_del(home_page.header);
    lv_scr_load(home_page.screen);

    lv_coord_t cell_w, cell_h, gap;
    create_grid_container(home_page.container, &cell_w, &cell_h, &gap);

    int app_count = app_manager_get_num_apps();

    if (app_count == 0) {
        ESP_LOGW(TAG, "No apps registered");
        lv_obj_t *lbl = lv_label_create(home_page.container);
        lv_label_set_text(lbl, "No apps installed");
        lv_obj_set_grid_cell(lbl, LV_GRID_ALIGN_CENTER, 0, 2,
                                  LV_GRID_ALIGN_CENTER, 0, 1);
        return;
    }

    int visible = 0;
    for (int i = 0; i < app_count; i++) {
        application_t *app = app_manager_get_app(i);
        if (!app || app->hidden) continue;

        int col = visible % 2;
        int row = visible / 2;
        create_app_grid_button(home_page.container, app, col, row, cell_w, gap);
        visible++;
    }

    if (visible == 0) {
        lv_obj_t *lbl = lv_label_create(home_page.container);
        lv_label_set_text(lbl, "All apps are hidden");
        lv_obj_set_grid_cell(lbl, LV_GRID_ALIGN_CENTER, 0, 2,
                                  LV_GRID_ALIGN_CENTER, 0, 1);
    }

    ESP_LOGI(TAG, "Home UI ready: %d visible apps", visible);
}
