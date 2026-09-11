#ifndef LV_HOME_INDICATOR_H
#define LV_HOME_INDICATOR_H

#include <lvgl.h>

/**
 * @brief 底部 Home 指示条 (iPhone 风格黑色中央横条)
 *
 * 在每个应用主界面底部创建一个居中的黑色圆角横条, 作为「上滑退出」手势的
 * 视觉提示。纯装饰、不拦截触摸 (clickable/scrollable 均已清除), 因此上滑手势
 * 仍由全局 launcher_gesture 处理。创建为 FLOATING 使其脱离父容器 flex 布局,
 * 始终对齐到父对象 (通常是 page.screen) 底部中央。
 *
 * @param parent 父对象 (通常为 page.screen)
 * @return 横条对象
 */
lv_obj_t *lv_home_indicator_create(lv_obj_t *parent);

#endif // LV_HOME_INDICATOR_H
