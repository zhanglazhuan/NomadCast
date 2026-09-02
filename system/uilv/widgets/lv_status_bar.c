/**
 * @file lv_status_bar.c
 * @brief 全局状态栏 — 单例，位于 lv_layer_top()，跨页面共享。
 *
 * 监听 app_event:
 *   APP_EVENT_CLOCK_TICK       → 更新时间 HH:MM
 *   APP_EVENT_WIFI_CONNECTED   → 显示 WiFi 图标
 *   APP_EVENT_WIFI_DISCONNECTED → 隐藏 WiFi 图标
 */
#include "lv_status_bar.h"
#include "app_event.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "status_bar";

/* ── 图标资源 ──────────────────────────────────────────────────────────── */
extern const lv_image_dsc_t ic_timer;
extern const lv_image_dsc_t ic_downloading;
extern const lv_image_dsc_t ic_wifi;
extern const lv_image_dsc_t ic_battery_android_0;
extern const lv_image_dsc_t ic_battery_android_1;
extern const lv_image_dsc_t ic_battery_android_2;
extern const lv_image_dsc_t ic_battery_android_3;
extern const lv_image_dsc_t ic_battery_android_4;
extern const lv_image_dsc_t ic_battery_android_5;
extern const lv_image_dsc_t ic_battery_android_6;
extern const lv_image_dsc_t ic_battery_android_full;
extern const lv_image_dsc_t ic_battery_android_bolt;

/* ── 内部常量 ───────────────────────────────────────────────────────────── */

#define TEXT_COLOR   0x333333
#define LINE_COLOR   0xCCCCCC
#define TEXT_FONT    &lv_font_montserrat_14
#define STATUS_ICON_SIZE 16   /* icons are 24×24 assets, scaled down to fit */

/* ── 全局单例 ────────────────────────────────────────────────────────────── */

static lv_status_bar_t *s_global_sb = NULL;

/* ── Event listener — handles time + WiFi ──────────────────────────────── */

static void on_app_event(app_event_t event, const void *data)
{
    if (!s_global_sb) return;

    switch (event) {
    case APP_EVENT_CLOCK_TICK: {
        const app_event_clock_tick_t *t = (const app_event_clock_tick_t *)data;
        if (t->synced) {
            lv_status_bar_update_time(s_global_sb, t->hour, t->minute);
        } else {
            lv_label_set_text(s_global_sb->time_label, "--:--");
        }
        break;
    }
    case APP_EVENT_WIFI_CONNECTED:
        lv_status_bar_set_wifi_connected(s_global_sb, true);
        break;
    case APP_EVENT_WIFI_DISCONNECTED:
        lv_status_bar_set_wifi_connected(s_global_sb, false);
        break;
    case APP_EVENT_DOWNLOAD_COMPLETED:
    case APP_EVENT_DOWNLOAD_CHANGED:
        /* Download state is driven by controller polling; nothing to do here. */
        break;
    case APP_EVENT_BATTERY_CHANGED: {
        const app_event_battery_t *b = (const app_event_battery_t *)data;
        lv_status_bar_update_battery(s_global_sb, b->percent, b->charging);
        break;
    }
    case APP_EVENT_KEY_PLAY_PAUSE:
    case APP_EVENT_KEY_VOL_UP:
    case APP_EVENT_KEY_VOL_DOWN:
        /* Physical keys — status bar has no reaction. */
        break;
    }
}

/* ── Battery icon mapping ───────────────────────────────────────────────── */

static const lv_image_dsc_t *get_battery_icon(int percentage, bool is_charging)
{
    if (is_charging) return &ic_battery_android_bolt;
    if (percentage >= 100) return &ic_battery_android_full;
    if (percentage >= 86)  return &ic_battery_android_6;
    if (percentage >= 72)  return &ic_battery_android_5;
    if (percentage >= 58)  return &ic_battery_android_4;
    if (percentage >= 43)  return &ic_battery_android_3;
    if (percentage >= 29)  return &ic_battery_android_2;
    if (percentage >= 15)  return &ic_battery_android_1;
    return &ic_battery_android_0;
}

/* ── Internal creation ──────────────────────────────────────────────────── */

/* Force an icon to render at STATUS_ICON_SIZE regardless of its source size.
 * CONTAIN keeps aspect ratio and scales the 24×24 asset down to fit the box —
 * also re-applies automatically when the battery icon swaps its src. */
static void size_status_icon(lv_obj_t *icon)
{
    lv_obj_set_size(icon, STATUS_ICON_SIZE, STATUS_ICON_SIZE);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
}

static lv_status_bar_t *create_status_bar(void)
{
    lv_status_bar_t *sb = lv_malloc(sizeof(lv_status_bar_t));
    if (!sb) return NULL;

    /* Root on lv_layer_top — floats above ALL screens */
    sb->bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(sb->bar, LV_PCT(100), LV_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_opa(sb->bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(sb->bar, lv_color_hex(LINE_COLOR), 0);
    lv_obj_set_style_border_side(sb->bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sb->bar, 1, 0);
    lv_obj_set_style_radius(sb->bar, 0, 0);
    lv_obj_set_style_pad_left(sb->bar, 8, 0);
    lv_obj_set_style_pad_right(sb->bar, 4, 0);
    lv_obj_set_style_pad_top(sb->bar, 0, 0);
    lv_obj_set_style_pad_bottom(sb->bar, 0, 0);
    lv_obj_set_scrollbar_mode(sb->bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(sb->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb->bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ── 左侧: 时间 ── */
    sb->time_label = lv_label_create(sb->bar);
    lv_label_set_text(sb->time_label, "--:--");
    lv_obj_set_style_text_color(sb->time_label, lv_color_hex(TEXT_COLOR), 0);
    lv_obj_set_style_text_font(sb->time_label, TEXT_FONT, 0);

    /* ── 右侧: 状态图标组 ── */
    lv_obj_t *icon_group = lv_obj_create(sb->bar);
    lv_obj_set_size(icon_group, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(icon_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_group, 0, 0);
    lv_obj_set_style_pad_all(icon_group, 0, 0);
    lv_obj_set_scrollbar_mode(icon_group, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(icon_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_group, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icon_group, 2, 0);

    /* 定时关机图标 (默认隐藏) */
    sb->timer_icon = lv_image_create(icon_group);
    lv_image_set_src(sb->timer_icon, &ic_timer);
    size_status_icon(sb->timer_icon);
    lv_obj_add_flag(sb->timer_icon, LV_OBJ_FLAG_HIDDEN);

    /* 下载速度文本 (默认隐藏, 位于下载图标左侧) */
    sb->speed_label = lv_label_create(icon_group);
    lv_label_set_text(sb->speed_label, "");
    lv_obj_set_style_text_color(sb->speed_label, lv_color_hex(TEXT_COLOR), 0);
    lv_obj_set_style_text_font(sb->speed_label, TEXT_FONT, 0);
    lv_obj_add_flag(sb->speed_label, LV_OBJ_FLAG_HIDDEN);

    /* 下载图标 (默认隐藏, 位于 WiFi 左侧) */
    sb->download_icon = lv_image_create(icon_group);
    lv_image_set_src(sb->download_icon, &ic_downloading);
    size_status_icon(sb->download_icon);
    lv_obj_add_flag(sb->download_icon, LV_OBJ_FLAG_HIDDEN);

    /* WiFi 图标 (默认隐藏) */
    sb->wifi_icon = lv_image_create(icon_group);
    lv_image_set_src(sb->wifi_icon, &ic_wifi);
    size_status_icon(sb->wifi_icon);
    lv_obj_add_flag(sb->wifi_icon, LV_OBJ_FLAG_HIDDEN);

    /* 电池图标 (默认满电) */
    sb->battery_icon = lv_image_create(icon_group);
    lv_image_set_src(sb->battery_icon, &ic_battery_android_full);
    size_status_icon(sb->battery_icon);

    /* Subscribe to all app events */
    app_event_register(on_app_event);

    ESP_LOGI(TAG, "Global status bar created on lv_layer_top");
    return sb;
}

/* ── 公共接口 ───────────────────────────────────────────────────────────── */

lv_status_bar_t *lv_status_bar_init(void)
{
    if (s_global_sb) return s_global_sb;
    s_global_sb = create_status_bar();
    return s_global_sb;
}

lv_status_bar_t *lv_status_bar_get(void)
{
    if (!s_global_sb) {
        s_global_sb = create_status_bar();
    }
    return s_global_sb;
}

lv_status_bar_t *lv_status_bar_create(lv_obj_t *parent)
{
    (void)parent;
    return lv_status_bar_get();
}

/* ── 更新函数 ───────────────────────────────────────────────────────────── */

void lv_status_bar_update_time(lv_status_bar_t *sb, int hour, int minute)
{
    if (!sb || !sb->time_label) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(sb->time_label, buf);
}

void lv_status_bar_set_timer_active(lv_status_bar_t *sb, bool active)
{
    if (!sb || !sb->timer_icon) return;
    if (active) lv_obj_clear_flag(sb->timer_icon, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(sb->timer_icon, LV_OBJ_FLAG_HIDDEN);
}

void lv_status_bar_set_download_active(lv_status_bar_t *sb, bool active)
{
    if (!sb || !sb->download_icon) return;
    if (active) lv_obj_clear_flag(sb->download_icon, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(sb->download_icon, LV_OBJ_FLAG_HIDDEN);
}

void lv_status_bar_set_download_speed(lv_status_bar_t *sb, int speed_bps)
{
    if (!sb || !sb->speed_label) return;
    if (speed_bps < 0) {
        lv_obj_add_flag(sb->speed_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int kbps = speed_bps * 8 / 1000;   /* bytes/s → kilobits/s */
    char buf[16];
    if (kbps < 1000)
        snprintf(buf, sizeof(buf), "%d kb/s", kbps);
    else
        snprintf(buf, sizeof(buf), "%.1f Mb/s", kbps / 1000.0);
    /* Only touch LVGL when the text actually changes — avoids redundant invalidation */
    if (strcmp(lv_label_get_text(sb->speed_label), buf) != 0)
        lv_label_set_text(sb->speed_label, buf);
    lv_obj_clear_flag(sb->speed_label, LV_OBJ_FLAG_HIDDEN);
}

void lv_status_bar_set_wifi_connected(lv_status_bar_t *sb, bool connected)
{
    if (!sb || !sb->wifi_icon) return;
    if (connected) lv_obj_clear_flag(sb->wifi_icon, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(sb->wifi_icon, LV_OBJ_FLAG_HIDDEN);
}

void lv_status_bar_update_battery(lv_status_bar_t *sb, int percentage, bool is_charging)
{
    if (!sb || !sb->battery_icon) return;
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    lv_image_set_src(sb->battery_icon, get_battery_icon(percentage, is_charging));
}
