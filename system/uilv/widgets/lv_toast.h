#ifndef LV_TOAST_H
#define LV_TOAST_H

#include <lvgl.h>

/**
 * @brief 显示 toast 提示，自动消失
 * @param message      提示文本
 * @param duration_ms  显示时长 (毫秒)，到时自动删除
 *
 * 外观: 黑色 75% 透明底 + 白色文字 + 圆角，底部居中
 */
void lv_toast_show(const char *message, int duration_ms);

#endif
