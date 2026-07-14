#ifndef LV_NUM_INPUT_H
#define LV_NUM_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

/**
 * @brief 创建数字输入组件
 * @param parent      父容器
 * @param title       标题 (NULL = 不显示)
 * @param initial_val 初始值
 * @param step        步长
 * @param unit        单位文本 (NULL = 不显示)
 * @return 组件根对象
 */
lv_obj_t * lv_number_input_create(lv_obj_t * parent, const char * title, int initial_val, int step, const char * unit);

/**
 * @brief 设置组件高度 (± 按钮和输入框统一缩放)
 * @param obj 组件根对象
 * @param h   目标高度 (默认 40)
 */
void lv_num_input_set_height(lv_obj_t * obj, lv_coord_t h);

#ifdef __cplusplus
}
#endif

#endif /* LV_NUM_INPUT_H */
