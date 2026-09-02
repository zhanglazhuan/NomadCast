#ifndef LV_STATUS_BAR_H
#define LV_STATUS_BAR_H

#include <lvgl.h>
#include <stdbool.h>

/** 状态栏高度 */
#define LV_STATUS_BAR_HEIGHT 24

/**
 * @brief 全局状态栏 — 单例，位于 lv_layer_top()，所有页面共享。
 *
 * 布局: [时间 HH:MM] ...弹性空间... [定时] [下载] [WiFi] [电池]
 *
 * 在 app_main 中调用 lv_status_bar_init() 一次即可。
 * 后续所有页面通过 lv_status_bar_get() 获取同一实例，
 * WiFi/电池/时间 状态在页面切换时自动保留。
 */
typedef struct {
    lv_obj_t *bar;              // 根容器 (LV_PCT(100) x 24)
    lv_obj_t *time_label;       // 时间文本 "HH:MM"
    lv_obj_t *timer_icon;       // 定时关机 (ic_timer)
    lv_obj_t *download_icon;    // 下载中  (ic_downloading)
    lv_obj_t *speed_label;      // 下载速度文本 (WiFi 左侧, 仅下载时显示)
    lv_obj_t *wifi_icon;        // WiFi     (ic_wifi)
    lv_obj_t *battery_icon;     // 电池     (动态: 0-6 bars / full / charging)
} lv_status_bar_t;

/**
 * @brief 初始化全局状态栏（仅需调用一次，例如在 app_main）。
 *        创建在 lv_layer_top() 上，所有页面自动可见。
 */
lv_status_bar_t *lv_status_bar_init(void);

/**
 * @brief 获取全局状态栏实例（如果没有初始化则自动创建）。
 */
lv_status_bar_t *lv_status_bar_get(void);

/** 已废弃 — 等效于 lv_status_bar_get()，忽略 parent 参数。 */
lv_status_bar_t *lv_status_bar_create(lv_obj_t *parent);

/** 更新时间 */
void lv_status_bar_update_time(lv_status_bar_t *sb, int hour, int minute);

/** 设置定时关机图标可见性 */
void lv_status_bar_set_timer_active(lv_status_bar_t *sb, bool active);

/** 设置下载图标可见性 */
void lv_status_bar_set_download_active(lv_status_bar_t *sb, bool active);

/** 设置下载速度文本 (bytes/sec)。传入 <0 隐藏标签；自动单位 KB/s / MB/s。 */
void lv_status_bar_set_download_speed(lv_status_bar_t *sb, int speed_bps);

/** 设置 WiFi 图标可见性 */
void lv_status_bar_set_wifi_connected(lv_status_bar_t *sb, bool connected);

/** 更新电池图标 */
void lv_status_bar_update_battery(lv_status_bar_t *sb, int percentage, bool is_charging);

#endif
