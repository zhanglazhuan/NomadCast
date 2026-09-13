#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "view_alarm_edit.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "alarm_service.h"
#include "lv_page.h"
#include "lang.h"

extern AlarmApp g_alarm_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    int editing_index;      /* -1 = new alarm */
    lv_obj_t *hour_roller;
    lv_obj_t *min_roller;
    int repeat;             /* alarm_repeat_t */
    uint8_t weekdays;       /* bit0=Sun … bit6=Sat */
    lv_obj_t *repeat_btns[3];
    lv_obj_t *repeat_lbls[3];
    lv_obj_t *wd_row;
    lv_obj_t *wd_btns[7];
    lv_obj_t *wd_lbls[7];
} AlarmEditCtx;

static void edit_ctx_cleanup(lv_event_t *e)
{
    AlarmEditCtx *ctx = (AlarmEditCtx *)lv_event_get_user_data(e);
    if (ctx) free(ctx);
}

static void build_options(char *buf, size_t n, int from, int to)
{
    buf[0] = '\0';
    for (int v = from; v <= to; v++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%02d\n", v);
        strncat(buf, tmp, n - strlen(buf) - 1);
    }
}

static void repeat_refresh(AlarmEditCtx *ctx)
{
    for (int i = 0; i < 3; i++) {
        bool sel = (i == ctx->repeat);
        lv_obj_set_style_bg_color(ctx->repeat_btns[i],
            sel ? lv_color_hex(0x1976D2) : lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_text_color(ctx->repeat_lbls[i],
            sel ? lv_color_white() : lv_color_hex(0x333333), 0);
    }
    if (ctx->wd_row) {
        if (ctx->repeat == ALARM_REPEAT_WEEKDAYS)
            lv_obj_clear_flag(ctx->wd_row, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ctx->wd_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wd_refresh(AlarmEditCtx *ctx)
{
    for (int i = 0; i < 7; i++) {
        bool sel = (ctx->weekdays & (1 << i)) != 0;
        lv_obj_set_style_bg_color(ctx->wd_btns[i],
            sel ? lv_color_hex(0x1976D2) : lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_text_color(ctx->wd_lbls[i],
            sel ? lv_color_white() : lv_color_hex(0x333333), 0);
    }
}

static void on_repeat_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    AlarmEditCtx *ctx = (AlarmEditCtx *)lv_event_get_user_data(e);
    if (!ctx) return;

    int mode = -1;
    for (int i = 0; i < 3; i++) if (ctx->repeat_btns[i] == btn) { mode = i; break; }
    if (mode < 0) return;

    ctx->repeat = mode;
    if (mode == ALARM_REPEAT_WEEKDAYS && ctx->weekdays == 0) {
        ctx->weekdays = 0x3E;   /* default Mon–Fri (bit1..5) */
    }
    repeat_refresh(ctx);
    wd_refresh(ctx);
}

static void on_wd_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    AlarmEditCtx *ctx = (AlarmEditCtx *)lv_event_get_user_data(e);
    if (!ctx) return;

    int wd = -1;
    for (int i = 0; i < 7; i++) if (ctx->wd_btns[i] == btn) { wd = i; break; }
    if (wd < 0) return;

    ctx->weekdays ^= (uint8_t)(1 << wd);
    wd_refresh(ctx);
}

static void on_save_clicked(lv_event_t *e)
{
    AlarmEditCtx *ctx = (AlarmEditCtx *)lv_event_get_user_data(e);
    if (!ctx) return;

    int h = (int)lv_roller_get_selected(ctx->hour_roller);
    int m = (int)lv_roller_get_selected(ctx->min_roller);
    uint8_t repeat = (uint8_t)ctx->repeat;
    uint8_t wd = (repeat == ALARM_REPEAT_WEEKDAYS) ? ctx->weekdays : 0;

    if (ctx->editing_index < 0) {
        alarm_service_add(h, m, true, repeat, wd);
    } else {
        const alarm_entry_t *cur = alarm_service_get(ctx->editing_index);
        bool en = cur ? cur->enabled : true;
        alarm_service_set(ctx->editing_index, h, m, en, repeat, wd);
    }

    page_navigator_navigate_pop(&g_alarm_app.view->page_nav, &g_alarm_app);
}

static void on_delete_clicked(lv_event_t *e)
{
    AlarmEditCtx *ctx = (AlarmEditCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->editing_index >= 0) {
        alarm_service_remove(ctx->editing_index);
    }
    page_navigator_navigate_pop(&g_alarm_app.view->page_nav, &g_alarm_app);
}

/* A full-width horizontal row of n equal toggle buttons. Button pointers are
 * written into out_btns and their labels into out_lbls. */
static lv_obj_t *make_button_row(lv_obj_t *parent, int n, lv_obj_t **out_btns,
                                 lv_obj_t **out_lbls, const char *const *labels,
                                 lv_event_cb_t cb, AlarmEditCtx *ctx, int h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, h);   /* explicit height — never rely on content size */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);

    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_height(b, h);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, g_cjk_font ? g_cjk_font : &lv_font_montserrat_14, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ctx);
        out_btns[i] = b;
        out_lbls[i] = l;
    }
    return row;
}

static lv_obj_t *build_alarm_edit_page(struct AlarmApp *app, void *user_data)
{
    int idx = (int)(intptr_t)user_data;
    if (app->model) app->model->editing_index = idx;

    const alarm_entry_t *cur = (idx >= 0) ? alarm_service_get(idx) : NULL;
    bool is_edit = (cur != NULL);
    int h = cur ? cur->hour : 7;
    int m = cur ? cur->minute : 0;

    Page page = lv_page_create(is_edit ? tr(STR_ALARM_EDIT) : tr(STR_ALARM_ADD),
                               true, page_navigator_navigate_back, &app->view->page_nav);
    lv_obj_t *cont = page.container;
    lv_obj_set_scroll_dir(cont, LV_DIR_NONE);   /* the whole page must never scroll */

    AlarmEditCtx *ctx = (AlarmEditCtx *)calloc(1, sizeof(AlarmEditCtx));
    ctx->editing_index = idx;
    ctx->repeat = cur ? cur->repeat : ALARM_REPEAT_DAILY;
    ctx->weekdays = cur ? cur->weekdays : 0;
    lv_obj_add_event_cb(page.screen, edit_ctx_cleanup, LV_EVENT_DELETE, ctx);

    /* Fixed-height stack (no flex-grow, no floating): time picker → repeat →
     * save/delete. 6px gaps, save/delete 6px from the bottom, no scrolling.
     * Every child has an explicit height so the layout is deterministic. */
    lv_obj_set_style_pad_top(cont, 8, 0);
    lv_obj_set_style_pad_bottom(cont, 6, 0);
    lv_obj_set_style_pad_left(cont, 12, 0);
    lv_obj_set_style_pad_right(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);

    /* Roller row: [HH] : [MM] */
    lv_obj_t *roller_row = lv_obj_create(cont);
    lv_obj_set_size(roller_row, LV_PCT(100), 116);
    lv_obj_set_style_border_width(roller_row, 0, 0);
    lv_obj_set_style_pad_all(roller_row, 0, 0);
    lv_obj_set_style_bg_opa(roller_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(roller_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(roller_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(roller_row, 8, 0);

    char hour_opts[200], min_opts[400];
    build_options(hour_opts, sizeof(hour_opts), 0, 23);
    build_options(min_opts, sizeof(min_opts), 0, 59);

    lv_obj_t *hr = lv_roller_create(roller_row);
    lv_roller_set_options(hr, hour_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(hr, 3);
    lv_roller_set_selected(hr, (uint32_t)h, LV_ANIM_OFF);
    lv_obj_set_scrollbar_mode(hr, LV_SCROLLBAR_MODE_OFF);
    ctx->hour_roller = hr;

    lv_obj_t *colon = lv_label_create(roller_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(colon, lv_color_hex(0x333333), 0);

    lv_obj_t *mr = lv_roller_create(roller_row);
    lv_roller_set_options(mr, min_opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(mr, 3);
    lv_roller_set_selected(mr, (uint32_t)m, LV_ANIM_OFF);
    lv_obj_set_scrollbar_mode(mr, LV_SCROLLBAR_MODE_OFF);
    ctx->min_roller = mr;

    /* Repeat mode: once / daily / weekdays */
    const char *repeat_labels[3] = {
        tr(STR_ALARM_REPEAT_ONCE),
        tr(STR_ALARM_REPEAT_DAILY),
        tr(STR_ALARM_REPEAT_WEEKDAYS),
    };
    make_button_row(cont, 3, ctx->repeat_btns, ctx->repeat_lbls,
                    repeat_labels, on_repeat_clicked, ctx, 34);

    /* Weekday selector (only when repeat == weekdays) */
    const char *wd_labels[7];
    for (int i = 0; i < 7; i++) wd_labels[i] = alarm_wd_short(i);
    ctx->wd_row = make_button_row(cont, 7, ctx->wd_btns, ctx->wd_lbls,
                                  wd_labels, on_wd_clicked, ctx, 34);

    /* Save / Delete — a fixed footer row, always on screen. */
    lv_obj_t *btn_row = lv_obj_create(cont);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, 44);   /* explicit height — never rely on content size */
    lv_obj_set_style_margin_top(btn_row, 2, 0);   /* 6px pad_row + 2px = 8px gap above */
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 8, 0);

    lv_obj_t *save = lv_button_create(btn_row);
    lv_obj_set_height(save, 44);
    lv_obj_set_flex_grow(save, is_edit ? 2 : 1);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x1976D2), LV_STATE_PRESSED);
    lv_obj_add_event_cb(save, on_save_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, tr(STR_ALARM_SAVE));
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl, g_cjk_font, 0);
    lv_obj_center(sl);

    if (is_edit) {
        lv_obj_t *del = lv_button_create(btn_row);
        lv_obj_set_height(del, 44);
        lv_obj_set_flex_grow(del, 1);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xE53935), 0);
        lv_obj_add_event_cb(del, on_delete_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, tr(STR_ALARM_DELETE));
        lv_obj_set_style_text_color(dl, lv_color_white(), 0);
        lv_obj_set_style_text_font(dl, g_cjk_font, 0);
        lv_obj_center(dl);
    }

    repeat_refresh(ctx);
    wd_refresh(ctx);

    return page.screen;
}

void alarm_view_edit_init_registry(struct AlarmApp *app) {
    PAGE_REGISTE(app, PAGE_EDIT, build_alarm_edit_page);
}
