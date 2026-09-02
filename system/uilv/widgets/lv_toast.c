/**
 * @file lv_toast.c
 * @brief Toast 提示 — 黑色半透明底 + 白色文字，自动消失
 */
#include "lv_toast.h"
#include "esp_log.h"

static const char *TAG = "toast";

/* Shared CJK font (registered by the app). Toast messages may contain Chinese,
 * which the ASCII-only montserrat font can't render — fall back only if unset. */
extern const struct _lv_font_t *g_cjk_font;

/* ── 定时器回调：自动删除 toast ─────────────────────────────────────────── */

static void toast_delete_cb(lv_timer_t *timer)
{
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (toast) {
        lv_obj_del(toast);
    }
    lv_timer_del(timer);
}

/* ── 公共接口 ───────────────────────────────────────────────────────────── */

void lv_toast_show(const char *message, int duration_ms)
{
    if (!message) return;

    /* 放在顶层，浮在所有内容之上 */
    lv_obj_t *toast = lv_label_create(lv_layer_top());

    /* 外观: 黑色 75% 不透明底 + 白色文字 + 圆角 */
    lv_obj_set_style_bg_color(toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(toast, 191 /* 75% */, 0);
    lv_obj_set_style_text_color(toast, lv_color_white(), 0);
    lv_obj_set_style_text_font(toast, g_cjk_font ? g_cjk_font : &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_pad_hor(toast, 20, 0);
    lv_obj_set_style_pad_ver(toast, 10, 0);

    /* 阴影增强层次感 */
    lv_obj_set_style_shadow_width(toast, 10, 0);
    lv_obj_set_style_shadow_color(toast, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(toast, LV_OPA_30, 0);

    lv_label_set_text(toast, message);

    /* 底部居中 */
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* 自动消失定时器 (回调中自删, 单次触发) */
    lv_timer_create(toast_delete_cb, (uint32_t)duration_ms, toast);

    ESP_LOGI(TAG, "\"%s\" (%d ms)", message, duration_ms);
}
