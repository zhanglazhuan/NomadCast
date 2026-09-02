/**
 * @file view_profile.c
 * @brief 我的页 — 用户信息 + 登录/登出 + 统计数据 + 菜单入口
 *
 * 登录状态枚举: LOGGED_OUT → 显示 login 图标, 点击弹出登录确认弹窗
 *              LOGGED_IN  → 显示 logout 图标, 点击弹出登出确认弹窗
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_profile.h"
#include "view_tab_bar.h"
#include "view_login.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../cache.h"
#include "lv_page.h"
#include "lv_bottom_sheet.h"

extern PodcastApp g_podcast_app;

/* 外部图标声明 */
extern const lv_image_dsc_t ic_logout;
extern const lv_image_dsc_t ic_login;

/* ── 功能开关:登录/登出图标按钮 ──
 * 0 = 隐藏按钮 (当前禁用登录), 1 = 显示 */
#define PROFILE_LOGIN_BTN  0

/* ---- 页面上下文 ---- */
typedef struct {
    lv_bottom_sheet_t* sheet;  // 当前弹窗, 同时只存在一个
} ProfilePageCtx;

#if PROFILE_LOGIN_BTN
static void on_sheet_delete(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    if (ctx) ctx->sheet = NULL;
}

static void on_sheet_cancel(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    if (ctx && ctx->sheet) { lv_bottom_sheet_close(ctx->sheet); ctx->sheet = NULL; }
}

/* ---- 登录: 弹出底部弹窗选择 Login 或 Register ---- */
static void on_sheet_choice(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    login_mode_t mode = (login_mode_t)(uintptr_t)lv_obj_get_user_data(
                            lv_event_get_current_target_obj(e));
    if (ctx && ctx->sheet) {
        lv_bottom_sheet_close(ctx->sheet);
        ctx->sheet = NULL;
    }
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_PROFILE, PAGE_LOGIN, (void*)(uintptr_t)mode);
}

static void on_login_clicked(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* scr = lv_screen_active();
    ctx->sheet = lv_bottom_sheet_create(scr);
    lv_obj_add_event_cb(ctx->sheet->overlay, on_sheet_delete, LV_EVENT_DELETE, ctx);

    lv_obj_t* content = lv_bottom_sheet_get_content(ctx->sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_style_pad_all(content, 16, 0);

    lv_obj_t* title = lv_label_create(content);
    lv_label_set_text(title, "Welcome");
    lv_obj_set_style_text_font(title, g_cjk_font, 0);

    lv_obj_t* btn_login = lv_button_create(content);
    lv_obj_set_size(btn_login, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(btn_login, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_radius(btn_login, 6, 0);
    lv_obj_set_user_data(btn_login, (void*)(uintptr_t)LOGIN_MODE_LOGIN);
    lv_obj_add_event_cb(btn_login, on_sheet_choice, LV_EVENT_CLICKED, ctx);
    lv_obj_t* bl = lv_label_create(btn_login);
    lv_label_set_text(bl, "Login");
    lv_obj_center(bl);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* btn_reg = lv_button_create(content);
    lv_obj_set_size(btn_reg, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(btn_reg, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_radius(btn_reg, 6, 0);
    lv_obj_set_user_data(btn_reg, (void*)(uintptr_t)LOGIN_MODE_REGISTER);
    lv_obj_add_event_cb(btn_reg, on_sheet_choice, LV_EVENT_CLICKED, ctx);
    lv_obj_t* br = lv_label_create(btn_reg);
    lv_label_set_text(br, "Register");
    lv_obj_center(br);
    lv_obj_set_style_text_color(br, lv_color_hex(0xFFFFFF), 0);
}

/* ---- 登出确认 ---- */
static void on_logout_confirm(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    if (ctx && ctx->sheet) { lv_bottom_sheet_close(ctx->sheet); ctx->sheet = NULL; }
    podcast_model_logout(&g_podcast_app);
    printf("[INF] Logout confirmed\n"); fflush(stdout);
    // 重建页面以刷新图标和状态
    page_navigator_navigate_to(&g_podcast_app.view->page_nav, &g_podcast_app, PAGE_PROFILE, NULL);
}

static void on_logout_clicked(lv_event_t* e) {
    ProfilePageCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* scr = lv_screen_active();
    ctx->sheet = lv_bottom_sheet_create(scr);
    lv_obj_add_event_cb(ctx->sheet->overlay, on_sheet_delete, LV_EVENT_DELETE, ctx);

    lv_obj_t* content = lv_bottom_sheet_get_content(ctx->sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 16, 0);

    lv_obj_t* msg = lv_label_create(content);
    lv_label_set_text(msg, "Are you sure logout?");
    lv_obj_set_style_text_color(msg, lv_color_hex(0x666666), 0);

    lv_obj_t* confirm = lv_button_create(content);
    lv_obj_set_size(confirm, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(confirm, 6, 0);
    lv_obj_add_event_cb(confirm, on_logout_confirm, LV_EVENT_CLICKED, ctx);
    lv_obj_t* cfl = lv_label_create(confirm);
    lv_label_set_text(cfl, "Logout");
    lv_obj_center(cfl);
    lv_obj_set_style_text_color(cfl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* cancel = lv_button_create(content);
    lv_obj_set_size(cancel, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_add_event_cb(cancel, on_sheet_cancel, LV_EVENT_CLICKED, ctx);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
}
#endif /* PROFILE_LOGIN_BTN */

/* ---- 页面导航回调 ---- */
static lv_timer_t *g_profile_dl_timer = NULL;

static void dl_count_refresh_cb(lv_timer_t *t) {
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(t);
    if (!label) { lv_timer_del(t); g_profile_dl_timer = NULL; return; }
    /* Check if label is still valid (not deleted) */
    if (lv_obj_is_valid(label)) {
        int count = podcast_model_get_pending_download_count(&g_podcast_app);
        if (count > 0)
            lv_label_set_text_fmt(label, "Download Task(%d)", count);
        else
            lv_label_set_text(label, "Download Task");
    } else {
        lv_timer_del(t);
        g_profile_dl_timer = NULL;
    }
}

static void on_profile_page_delete(lv_event_t *e) {
    if (g_profile_dl_timer) { lv_timer_del(g_profile_dl_timer); g_profile_dl_timer = NULL; }
}

static void on_download_task_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_PROFILE, PAGE_DOWNLOAD_TASK, NULL);
}

static void on_settings_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_PROFILE, PAGE_SETTINGS, NULL);
}

/* ---- 页面构建 ---- */
static lv_obj_t* build_profile_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;

    /* 页面上下文 (生命周期同页面) */
    if (app->view->page_nav.nav_ctx) free(app->view->page_nav.nav_ctx);
    ProfilePageCtx* ctx = malloc(sizeof(ProfilePageCtx));
    memset(ctx, 0, sizeof(ProfilePageCtx));
    app->view->page_nav.nav_ctx = ctx;

    Page page = lv_page_create(NULL, false, NULL, NULL);
    lv_obj_set_flex_grow(page.container, 1);  /* 填满剩余高度 */
    lv_obj_set_style_bg_color(page.screen, lv_color_hex(0xF5F5F5), 0);

    /* 内容区 — 垂直可滚动 */
    lv_obj_t* main = page.container;
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(main, LV_DIR_VER);
    lv_obj_set_style_bg_opa(main, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(main, 4, 0);
    lv_obj_set_style_pad_top(main, 0, 0);
    lv_obj_set_style_pad_row(main, 6, 0);

    /* ---- 用户信息卡片 ---- */
    lv_obj_t* card = lv_obj_create(main);
    lv_obj_set_size(card, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);

    lv_obj_t* info = lv_obj_create(card);
    lv_obj_set_size(info, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);

    /* 用户信息 — 从 model 读取 */
    bool logged_in = podcast_model_is_logged_in(app);
    const char* username = podcast_model_get_username(app);
    const char* user_id  = podcast_model_get_user_id(app);

    lv_obj_t* name = lv_label_create(info);
    lv_label_set_text(name, logged_in ? username : "Not logged in");
    lv_obj_set_style_text_font(name, g_cjk_font, 0);

    lv_obj_t* uid = lv_label_create(info);
    lv_label_set_text(uid, logged_in ? user_id : "Tap to login");
    lv_obj_set_style_text_color(uid, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(uid, g_cjk_font, 0);

#if PROFILE_LOGIN_BTN
    /* 右侧图标按钮 — 按登录状态切换 login / logout */
    lv_obj_t* icon_btn = lv_button_create(card);
    lv_obj_set_size(icon_btn, 36, 36);
    lv_obj_set_style_bg_opa(icon_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_btn, 0, 0);
    lv_obj_set_style_shadow_width(icon_btn, 0, 0);

    lv_obj_t* icon_img = lv_image_create(icon_btn);
    lv_obj_center(icon_img);
    lv_obj_set_style_img_recolor_opa(icon_img, LV_OPA_COVER, 0);

    if (!logged_in) {
        lv_image_set_src(icon_img, &ic_login);
        lv_obj_set_style_img_recolor(icon_img, lv_color_hex(0x999999), 0);
        lv_obj_add_event_cb(icon_btn, on_login_clicked, LV_EVENT_CLICKED, ctx);
    } else {
        lv_image_set_src(icon_img, &ic_logout);
        lv_obj_set_style_img_recolor(icon_img, lv_color_hex(0x999999), 0);
        lv_obj_add_event_cb(icon_btn, on_logout_clicked, LV_EVENT_CLICKED, ctx);
    }
#endif /* PROFILE_LOGIN_BTN */

    /* ---- 统计数据 ---- */
    lv_obj_t* stats = lv_obj_create(main);
    lv_obj_set_size(stats, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(stats, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(stats, 0, 0);
    lv_obj_set_style_radius(stats, 8, 0);
    lv_obj_set_style_pad_all(stats, 0, 0);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    /* 真实统计数据: 播放时长(累加各集播放位置) + 已下载集数 */
    int play_hours = cache_playback_total_sec() / 3600;
    int downloads  = app->model ? app->model->local_episode_count : 0;
    char play_val[16], dl_val[16];
    snprintf(play_val, sizeof(play_val), "%d", play_hours);
    snprintf(dl_val, sizeof(dl_val), "%d", downloads);

    struct { const char* val; const char* label; } stat_data[] = {
        {play_val, "Play Hours"}, {dl_val, "Downloads"},
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t* item = lv_obj_create(stats);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_row(item, 0, 0);

        lv_obj_t* v = lv_label_create(item);
        lv_label_set_text(v, stat_data[i].val);
        lv_obj_set_style_text_font(v, g_cjk_font, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0x1976D2), 0);

        lv_obj_t* l = lv_label_create(item);
        lv_label_set_text(l, stat_data[i].label);
        lv_obj_set_style_text_color(l, lv_color_hex(0x888888), 0);
    }

    /* ---- 菜单项 ---- */

    int pend_count = podcast_model_get_pending_download_count(&g_podcast_app);
    char dt_label[64];
    if (pend_count > 0)
        snprintf(dt_label, sizeof(dt_label), "Download Task(%d)", pend_count);
    else
        snprintf(dt_label, sizeof(dt_label), "Download Task");

    lv_obj_t* dt_btn = lv_button_create(main);
    lv_obj_set_size(dt_btn, LV_PCT(100), 42);
    lv_obj_set_style_bg_color(dt_btn, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(dt_btn, 0, 0);
    lv_obj_set_style_radius(dt_btn, 8, 0);
    lv_obj_add_event_cb(dt_btn, on_download_task_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* dt_t = lv_label_create(dt_btn);
    lv_label_set_text(dt_t, dt_label);
    lv_obj_set_style_text_color(dt_t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(dt_t);

    /* Refresh download count every 2 seconds */
    if (g_profile_dl_timer) lv_timer_del(g_profile_dl_timer);
    g_profile_dl_timer = lv_timer_create(dl_count_refresh_cb, 2000, (void *)dt_t);
    /* Clean up timer when page is destroyed */
    lv_obj_add_event_cb(page.screen, on_profile_page_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t* settings_btn = lv_button_create(main);
    lv_obj_set_size(settings_btn, LV_PCT(100), 42);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(settings_btn, 0, 0);
    lv_obj_set_style_radius(settings_btn, 8, 0);
    lv_obj_add_event_cb(settings_btn, on_settings_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* st = lv_label_create(settings_btn);
    lv_label_set_text(st, "Settings");
    lv_obj_set_style_text_color(st, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(st);

    podcast_view_create_bottom_tab_bar(page.screen, TAB_PROFILE);
    printf("[INF] Profile page built (state=%s)\n",
           podcast_model_is_logged_in(app) ? "in" : "out");
    fflush(stdout);
    return page.screen;
}

void podcast_view_profile_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_PROFILE, build_profile_page);
}
