/**
 * @file view_about.c
 * @brief 关于本机 — 显示设备ID / 名称 / 版本号等
 */
#include <stdio.h>
#include <string.h>
#include "view_about.h"
#include "../view.h"
#include "../app.h"
#include "lv_page.h"
#include "flash_store.h"

extern SettingsApp g_settings_app;

static lv_obj_t* build_about_page(struct SettingsApp *app, void *user_data)
{
    (void)user_data;
    Page page = lv_page_create("About", true, page_navigator_navigate_back,
                               &app->view->page_nav);

    lv_obj_set_style_bg_color(page.container, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_pad_all(page.container, 12, 0);
    lv_obj_set_style_pad_row(page.container, 4, 0);
    lv_obj_set_scroll_dir(page.container, LV_DIR_VER);

    /* Read device ID from NVS (set by podcast model on first boot) */
    char dev_id[32];
    flash_get_str("podcast", "devid", dev_id, sizeof(dev_id), "N/A");

    struct {
        const char *label;
        const char *value;
    } items[] = {
        {"Device Name",  "NomadCast"},
        {"Device ID",    dev_id},
        {"Firmware Ver", "0.1.0"},
        {"SDK",          "ESP-IDF v5.5.3"},
        {"Hardware",     "Leisound V1 (ESP32-S3)"},
        {"LVGL",         "v9.5"},
    };

    for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
        lv_obj_t *row = lv_obj_create(page.container);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);

        lv_obj_t *lb = lv_label_create(row);
        lv_label_set_text(lb, items[i].label);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x999999), 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, items[i].value);
        lv_obj_set_style_text_color(val, lv_color_hex(0x333333), 0);
    }

    return page.screen;
}

void settings_view_about_init_registry(struct SettingsApp *app)
{
    PAGE_REGISTE(app, SETTINGS_PAGE_ABOUT, build_about_page);
}
