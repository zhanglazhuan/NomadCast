/**
 * @file lv_num_input.c
 * @brief Number input widget (LVGL v9 API)
 */
#include <stdio.h>
#include "lvgl.h"
#include "lv_num_input.h"

typedef struct {
    lv_obj_t * main_cont;
    lv_obj_t * ta;
    lv_obj_t * btn_plus;
    lv_obj_t * btn_minus;
    int step;
    int current_val;
    lv_coord_t item_h;
    bool updating;  /* 防重入标志 */
} lv_num_input_ctx_t;

static void ctx_store_cb(lv_event_t * e) { (void)e; }

static void notify_value_changed(lv_num_input_ctx_t * ctx, int new_val) {
    if (ctx && ctx->main_cont)
        lv_obj_send_event(ctx->main_cont, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)new_val);
}

static void step_btn_event_cb(lv_event_t * e) {
    lv_num_input_ctx_t * ctx = lv_event_get_user_data(e);
    lv_obj_t * target = lv_event_get_current_target(e);
    if (!ctx) return;
    int delta = (target == ctx->btn_plus) ? ctx->step : -(ctx->step);
    ctx->current_val += delta;
    if (ctx->current_val < 0) ctx->current_val = 0;
    if (ctx->ta) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", ctx->current_val);
        ctx->updating = true;
        lv_textarea_set_text(ctx->ta, buf);
        ctx->updating = false;
    }
    notify_value_changed(ctx, ctx->current_val);
}

static void ta_event_cb(lv_event_t * e) {
    lv_num_input_ctx_t * ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->ta) return;
    if (ctx->updating) return;  /* 程序触发的 set_text，跳过 */
    const char * txt = lv_textarea_get_text(ctx->ta);
    if (!txt || !txt[0]) return;
    int val = atoi(txt);
    if (val < 0) { val = 0; lv_textarea_set_text(ctx->ta, "0"); }
    ctx->current_val = val;
    notify_value_changed(ctx, val);
}

static void component_delete_cb(lv_event_t * e) {
    lv_num_input_ctx_t * ctx = lv_event_get_user_data(e);
    if (ctx) lv_free(ctx);
}

lv_obj_t * lv_number_input_create(lv_obj_t * parent, const char * title, int initial_val, int step, const char * unit) {
    lv_num_input_ctx_t * ctx = lv_malloc(sizeof(lv_num_input_ctx_t));
    if (!ctx) return NULL;
    ctx->step = step;
    ctx->current_val = initial_val;
    ctx->item_h = 40; // 默认高度
    ctx->updating = false;

    ctx->main_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(ctx->main_cont);
    lv_obj_set_size(ctx->main_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->main_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->main_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(ctx->main_cont, component_delete_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(ctx->main_cont, ctx_store_cb, LV_EVENT_LAST, ctx);

    if (title) {
        lv_obj_t * lt = lv_label_create(ctx->main_cont);
        lv_label_set_text(lt, title);
    }

    lv_obj_t * input_cont = lv_obj_create(ctx->main_cont);
    lv_obj_remove_style_all(input_cont);
    lv_obj_set_size(input_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ctx->btn_minus = lv_button_create(input_cont);
    lv_obj_set_width(ctx->btn_minus, 40); // 高度由后面统一管理
    lv_obj_t * lm = lv_label_create(ctx->btn_minus);
    lv_label_set_text_fmt(lm, "-%d", step);
    lv_obj_center(lm);
    lv_obj_add_event_cb(ctx->btn_minus, step_btn_event_cb, LV_EVENT_CLICKED, ctx);

    ctx->ta = lv_textarea_create(input_cont);
    lv_textarea_set_one_line(ctx->ta, true);
    lv_obj_set_width(ctx->ta, 60); // 高度由后面统一管理
    lv_obj_set_style_text_align(ctx->ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_textarea_set_accepted_chars(ctx->ta, "0123456789");
    lv_textarea_set_cursor_click_pos(ctx->ta, true);
    lv_obj_clear_flag(ctx->ta, LV_OBJ_FLAG_SCROLLABLE);
    
    // 【新增修复点】消除闪烁光标块本身的物理宽高对居中对齐带来的高频抖动干扰
    lv_obj_set_style_width(ctx->ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_pad_all(ctx->ta, 0, LV_PART_CURSOR);

    char buf[16]; snprintf(buf, sizeof(buf), "%d", initial_val);
    lv_textarea_set_text(ctx->ta, buf);
    lv_obj_add_event_cb(ctx->ta, ta_event_cb, LV_EVENT_READY, ctx);

    ctx->btn_plus = lv_button_create(input_cont);
    lv_obj_set_width(ctx->btn_plus, 40); // 高度由后面统一管理
    lv_obj_t * lp = lv_label_create(ctx->btn_plus);
    lv_label_set_text_fmt(lp, "+%d", step);
    lv_obj_center(lp);
    lv_obj_add_event_cb(ctx->btn_plus, step_btn_event_cb, LV_EVENT_CLICKED, ctx);

    if (unit) {
        lv_obj_t * lu = lv_label_create(ctx->main_cont);
        lv_label_set_text(lu, unit);
    }

    // 【新增修复点】初始化完成时，主动调用一次逻辑高度控制，确保默认 40 高度下也应用完美缩排
    lv_num_input_set_height(ctx->main_cont, ctx->item_h);

    return ctx->main_cont;
}

void lv_num_input_set_height(lv_obj_t * obj, lv_coord_t h) {
    if (!obj) return;
    lv_num_input_ctx_t * ctx = NULL;
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n && !ctx; i++) {
        lv_event_dsc_t * dsc = lv_obj_get_event_dsc(obj, i);
        if (!dsc) continue;
        if (lv_event_dsc_get_cb(dsc) == ctx_store_cb)
            ctx = lv_event_dsc_get_user_data(dsc);
    }
    if (!ctx) return;
    ctx->item_h = h;

    // 1. 两侧加减按钮安全应用固定物理高度
    lv_obj_set_height(ctx->btn_minus, h);
    lv_obj_set_height(ctx->btn_plus,  h);

    // 2. 对中间文本框进行动态安全缩排（核心逻辑修复）
    const lv_font_t * font = lv_obj_get_style_text_font(ctx->ta, LV_PART_MAIN);
    int32_t font_h = font ? lv_font_get_line_height(font) : 16;
    int32_t border_w = lv_obj_get_style_border_width(ctx->ta, LV_PART_MAIN);

    // 精确推算：为了满足外部期望的目标总高度 h，内部纯文本上下两端所需的内边距
    int32_t total_pad = h - font_h - (border_w * 2);
    if (total_pad < 0) total_pad = 0;

    int32_t pad_top = total_pad / 2;
    int32_t pad_bottom = total_pad - pad_top;

    // 动态重设输入框的 Padding
    lv_obj_set_style_pad_top(ctx->ta, pad_top, 0);
    lv_obj_set_style_pad_bottom(ctx->ta, pad_bottom, 0);
    lv_obj_set_style_pad_left(ctx->ta, 4, 0);
    lv_obj_set_style_pad_right(ctx->ta, 4, 0);

    // 绝杀：将输入框改回 LV_SIZE_CONTENT！
    // 配合上面刚刚算好的 Padding，它的最终外壳物理高度会精确等于 h
    // 并且因为高度是 CONTENT 自适应，内部纵向滚动行程（max_scroll_y）被死死卡在 0 像素
    // 从而在物理层面彻底剥夺了文本框内部上下乱跳的可能。
    lv_obj_set_height(ctx->ta, LV_SIZE_CONTENT);
}