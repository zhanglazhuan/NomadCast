/**
 * @file view_login.c
 * @brief 登录页 — Name / Password / Confirm Password / 协议勾选 / Login 按钮
 *
 * 键盘行为：
 *   - 点击输入框 → 弹出键盘
 *   - 点击非输入框区域 → 输入框失焦 → 隐藏键盘
 *   - 点击键盘左下角模式键 → 切换键盘模式
 *   - 再点击另一个输入框 → 键盘切换到该输入框
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_login.h"
#include "../view.h"
#include "../app.h"
#include "../controller.h"
#include "lv_page.h"
#include "lv_toast.h"

extern PodcastApp g_podcast_app;

/* ── 表单上下文 ────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t* name_input;
    lv_obj_t* pwd_input;
    lv_obj_t* cpwd_input;
    lv_obj_t* cpwd_label;      /* "Confirm Password" label (hidden in LOGIN mode) */
    lv_obj_t* agree_cb;
    lv_obj_t* kb;
    login_mode_t mode;         /* LOGIN or REGISTER */
} LoginFormCtx;

/* 全局上下文 — indev filter 需要访问 */
static LoginFormCtx *g_active_login_ctx = NULL;

/* ── 键盘显隐 ─────────────────────────────────────────────────────────── */

static void hide_keyboard(LoginFormCtx *ctx) {
    if (!ctx || !ctx->kb) return;
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
}

static void attach_keyboard(LoginFormCtx *ctx, lv_obj_t *ta, lv_obj_t *cont) {
    if (!ctx || !ctx->kb || !ta) return;
    lv_obj_clear_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(ctx->kb, ta);
}

/* ── indev 触摸过滤器 — 隐藏键盘，不参与 show（show 由 focus 处理） ───── */

static void indev_press_filter(lv_event_t *e) {
    LoginFormCtx *ctx = g_active_login_ctx;
    if (!ctx || !ctx->kb || !lv_obj_is_valid(ctx->kb)) return;
    if (lv_obj_has_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN)) return;

    lv_point_t pt;
    lv_indev_get_point(lv_event_get_target(e), &pt);

    /* 点在键盘区域内 → 检测左下角和右下角 */
    lv_area_t kb_area;
    lv_obj_get_coords(ctx->kb, &kb_area);
    if (pt.x >= kb_area.x1 && pt.x <= kb_area.x2 &&
        pt.y >= kb_area.y1 && pt.y <= kb_area.y2) {
        int kb_w = kb_area.x2 - kb_area.x1;
        int kb_h = kb_area.y2 - kb_area.y1;

        /* 左下角 — 隐藏键盘 */
        if (pt.x < kb_area.x1 + kb_w / 4 && pt.y > kb_area.y2 - kb_h / 4) {
            hide_keyboard(ctx);
        }
        /* 右下角 ✓ — 由 LV_EVENT_READY 处理，不在这里重复 */
        return;
    }

    /* 点在 textarea 上 — 由 focus 事件处理，这里不干预 */
    lv_obj_t *tas[3] = { ctx->name_input, ctx->pwd_input, ctx->cpwd_input };
    for (int i = 0; i < 3; i++) {
        lv_area_t ta_area;
        lv_obj_get_coords(tas[i], &ta_area);
        if (pt.x >= ta_area.x1 && pt.x <= ta_area.x2 &&
            pt.y >= ta_area.y1 && pt.y <= ta_area.y2) return;
    }

    /* 点在上述以外区域 → 隐藏键盘 */
    hide_keyboard(ctx);
}

/* ── textarea 事件 ────────────────────────────────────────────────────── */

static void on_ta_focused(lv_event_t *e) {
    LoginFormCtx *ctx = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    attach_keyboard(ctx, ta, lv_obj_get_parent(ta));
}

static void on_ta_defocused(lv_event_t *e) {
    LoginFormCtx *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->kb) return;
    lv_obj_add_flag(ctx->kb, LV_OBJ_FLAG_HIDDEN);
}

/* ── 键盘 OK 键 → 切到下一个输入框 ────────────────────────────────────── */

static void on_ta_ready(lv_event_t *e) {
    LoginFormCtx *ctx = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *next = NULL;
    if (ta == ctx->name_input) {
        next = ctx->pwd_input;
    } else if (ta == ctx->pwd_input && ctx->mode == LOGIN_MODE_REGISTER) {
        next = ctx->cpwd_input;
    }
    if (next) {
        lv_obj_clear_state(ta,   LV_STATE_FOCUSED);
        lv_obj_add_state(next,    LV_STATE_FOCUSED);
        attach_keyboard(ctx, next, lv_obj_get_parent(next));
    }
}

/* ── 登录按钮 ──────────────────────────────────────────────────────────── */

static void on_login_btn_clicked(lv_event_t* e) {
    LoginFormCtx* ctx = lv_event_get_user_data(e);

    const char* name    = lv_textarea_get_text(ctx->name_input);
    const char* pwd     = lv_textarea_get_text(ctx->pwd_input);
    /* In LOGIN mode, confirm password is the same as password (no check) */
    const char* confirm = (ctx->mode == LOGIN_MODE_REGISTER)
                          ? lv_textarea_get_text(ctx->cpwd_input) : pwd;
    bool agreed = lv_obj_has_state(ctx->agree_cb, LV_STATE_CHECKED);

    const char* err_msg = NULL;
    bool ok = podcast_controller_login(&g_podcast_app,
                                      name, pwd, confirm, agreed,
                                      &err_msg);
    if (ok) {
        printf("[INF] %s succeeded\n",
               ctx->mode == LOGIN_MODE_REGISTER ? "Register" : "Login");
        fflush(stdout);
        page_navigator_navigate_pop(&g_podcast_app.view->page_nav, &g_podcast_app);
    } else {
        lv_toast_show(err_msg, 2000);
        printf("[WARN] %s failed: %s\n",
               ctx->mode == LOGIN_MODE_REGISTER ? "Register" : "Login", err_msg);
        fflush(stdout);
    }
}

/* ── 页面销毁 ──────────────────────────────────────────────────────────── */

static void ctx_cleanup(lv_event_t *e) {
    LoginFormCtx *ctx = lv_event_get_user_data(e);
    /* Detach indev filter to prevent use-after-free.
     * remove by matching the callback function pointer (only one was added). */
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_remove_event_cb_with_user_data(indev, indev_press_filter, NULL);
    g_active_login_ctx = NULL;
    if (ctx) free(ctx);
}

/* ── 单行输入框 ────────────────────────────────────────────────────────── */

static lv_obj_t* create_text_input(lv_obj_t* parent, const char* placeholder,
                                   bool is_password, LoginFormCtx* ctx) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(100), 40);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 8, 0);
    lv_obj_set_style_text_font(ta, g_cjk_font, 0);

    if (is_password) {
        lv_textarea_set_password_mode(ta, true);
        lv_textarea_set_password_bullet(ta, "*");
    }

    lv_obj_add_event_cb(ta, on_ta_focused,  LV_EVENT_FOCUSED,   ctx);
    lv_obj_add_event_cb(ta, on_ta_defocused, LV_EVENT_DEFOCUSED, ctx);
    lv_obj_add_event_cb(ta, on_ta_ready,     LV_EVENT_READY,     ctx);
    return ta;
}

/* ── 页面构建 ──────────────────────────────────────────────────────────── */

static lv_obj_t* build_login_page(struct PodcastApp* app, void* user_data) {
    login_mode_t mode = (login_mode_t)(uintptr_t)user_data;
    bool is_register = (mode == LOGIN_MODE_REGISTER);

    Page page = lv_page_create(is_register ? "Register" : "Login",
                               true, page_navigator_navigate_back,
                               &app->view->page_nav);

    lv_obj_set_style_bg_color(page.container, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_pad_all(page.container, 12, 0);
    lv_obj_set_style_pad_row(page.container, 6, 0);
    lv_obj_set_scroll_dir(page.container, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(page.container, LV_SCROLLBAR_MODE_OFF);

    LoginFormCtx* ctx = malloc(sizeof(LoginFormCtx));
    memset(ctx, 0, sizeof(LoginFormCtx));
    ctx->mode = mode;
    lv_obj_add_event_cb(page.screen, ctx_cleanup, LV_EVENT_DELETE, ctx);

    /* Name */
    lv_obj_t* nl = lv_label_create(page.container);
    lv_label_set_text(nl, "Name");
    lv_obj_set_style_text_font(nl, g_cjk_font, 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(0x333333), 0);
    ctx->name_input = create_text_input(page.container, "Enter your name", false, ctx);

    /* Password */
    lv_obj_t* pl = lv_label_create(page.container);
    lv_label_set_text(pl, "Password");
    lv_obj_set_style_text_font(pl, g_cjk_font, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(0x333333), 0);
    ctx->pwd_input = create_text_input(page.container, "Enter password", true, ctx);

    /* Confirm Password — visible only in REGISTER mode */
    lv_obj_t* cpl = lv_label_create(page.container);
    lv_label_set_text(cpl, "Confirm Password");
    lv_obj_set_style_text_font(cpl, g_cjk_font, 0);
    lv_obj_set_style_text_color(cpl, lv_color_hex(0x333333), 0);
    ctx->cpwd_label = cpl;
    ctx->cpwd_input = create_text_input(page.container, "Re-enter password", true, ctx);
    if (!is_register) {
        lv_obj_add_flag(cpl,               LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ctx->cpwd_input,   LV_OBJ_FLAG_HIDDEN);
    }

    /* 协议勾选 */
    ctx->agree_cb = lv_checkbox_create(page.container);
    lv_checkbox_set_text(ctx->agree_cb, "I agree to the Terms of Service");
    lv_obj_set_style_pad_all(ctx->agree_cb, 0, 0);

    /* Login 按钮 */
    lv_obj_t* login_btn = lv_button_create(page.container);
    lv_obj_set_size(login_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);
    lv_obj_set_style_radius(login_btn, 8, 0);
    lv_obj_add_event_cb(login_btn, on_login_btn_clicked, LV_EVENT_CLICKED, ctx);

    lv_obj_t* login_label = lv_label_create(login_btn);
    lv_label_set_text(login_label, "Login");
    lv_obj_center(login_label);
    lv_obj_set_style_text_color(login_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(login_label, g_cjk_font, 0);

    /* 键盘 (floating, 初始隐藏, 三个输入框共享) */
    lv_obj_t* kb = lv_keyboard_create(page.screen);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    ctx->kb = kb;

    /* indev 级别触摸过滤:
     *   - 点键盘左下角 → 隐藏键盘
     *   - 点键盘右下角 → 切到下一个输入框
     *   - 点键盘/输入框以外 → 隐藏键盘
     * 不处理 show（show 由 LV_EVENT_FOCUSED 负责），避免干扰键盘按键。 */
    g_active_login_ctx = ctx;
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_add_event_cb(indev, indev_press_filter, LV_EVENT_PRESSED, NULL);

    printf("[INF] Login page built\n"); fflush(stdout);
    return page.screen;
}

void podcast_view_login_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_LOGIN, build_login_page);
}
