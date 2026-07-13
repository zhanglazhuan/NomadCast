/**
 * @file view_local.c
 * @brief 本地页 — controller 检查 SD 卡 + 已下载内容，按状态展示
 *
 * 流程: view → controller → hal (SD 卡) + model (已下载计数)
 *       SD 缺失 → 提示 "No SD card inserted, no local content."
 *       无内容   → 提示 "No audio downloaded yet. Please go to Network to download."
 *       有内容   → 分类 Tab + 专辑卡片
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_local.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "app_event.h"
#include "lv_page.h"
#include "../lv_channel_card.h"

extern PodcastApp g_podcast_app;
extern const lv_image_dsc_t ic_search;

typedef struct {
    lv_obj_t* tabview;
    lv_obj_t* tab_pages[CHANNEL_CATEGORY_COUNT];
    uint32_t  build_gen;   /* bumped by refresh_list; stale chunk builds self-abort */
} LocalPageCtx;

static const char* CAT_NAMES[CHANNEL_CATEGORY_COUNT] = {
    "时事", "科技", "人文", "生活", "教育", "其他",
};

/* Live Local page tracking for download-complete refresh.
 * The event bus has no unregister, so we register once and validate liveness. */
static lv_obj_t*     s_local_screen   = NULL;  /* live Local screen, or NULL */
static LocalPageCtx* s_local_ctx      = NULL;  /* set in content mode; NULL in hint mode */
static bool          s_evt_registered = false;

static void on_search_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_LOCAL, PAGE_SEARCH, NULL);
}

/* ── 点击处理 ────────────────────────────────────────────────────────── */

static void on_card_clicked(lv_event_t* e) {
    lv_obj_t* card = lv_event_get_current_target_obj(e);
    int channel_id = (int)(uintptr_t)lv_obj_get_user_data(card);
    printf("[INF] Local card clicked, channel_id=%d\n", channel_id); fflush(stdout);
    int* id_ptr = malloc(sizeof(int));
    *id_ptr = channel_id;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_LOCAL, PAGE_CHANNEL, id_ptr);
}

/* ---- "Network" 链接点击 ---- */
static void on_network_link_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_LOCAL, PAGE_NETWORK, NULL);
}

/* ---- 悬浮搜索按钮 ---- */
#define FAB_SIZE  40
#define FAB_MARGIN_RIGHT 12

static void create_search_fab(lv_obj_t* screen) {
    lv_obj_t* fab = lv_button_create(screen);
    lv_obj_set_size(fab, FAB_SIZE, FAB_SIZE);
    lv_obj_set_style_radius(fab, FAB_SIZE / 2, 0);
    lv_obj_set_style_bg_color(fab, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(fab, 0, 0);
    lv_obj_set_style_shadow_width(fab, 20, 0);
    lv_obj_set_style_shadow_color(fab, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(fab, LV_OPA_30, 0);
    lv_obj_add_flag(fab, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(fab, on_search_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -FAB_MARGIN_RIGHT, -48);

    lv_obj_t* img = lv_image_create(fab);
    lv_image_set_src(img, &ic_search);
    lv_obj_center(img);
    lv_obj_set_style_img_recolor(img, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
}

/* ---- 提示页 (SD 缺失 / 无内容) ---- */
static void build_hint(lv_obj_t* parent, const char* msg, bool show_network_link) {
    /* Explicit pixel size: avoid lv_text_get_size_attributes + CJK font hang.
     * Height fits up to ~3 wrapped lines so the full hint isn't clipped. */
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label, g_cjk_font, 0);
    lv_obj_set_size(label, 200, 60);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, show_network_link ? -24 : 0);

    if (show_network_link) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, 32);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1976D2), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_pad_hor(btn, 16, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_network_link_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 44);

        lv_obj_t* btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, "Go to Network");
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(btn_label, g_cjk_font, 0);
        lv_obj_center(btn_label);
    }
}

/* ── Chunked card creation (shared type with view_network.c) ──────────────── */

#define CARDS_PER_TICK 2

typedef struct {
    lv_obj_t      *row;
    const Channel **albums;
    int             count;
    int             card_h;
    int             next;
    LocalPageCtx   *owner;    /* page ctx that spawned this build */
    uint32_t        gen;      /* generation at spawn; compared to owner->build_gen */
} card_build_ctx_t;

static void build_card_chunk_cb(lv_timer_t *timer) {
    card_build_ctx_t *bc = (card_build_ctx_t *)lv_timer_get_user_data(timer);
    if (!bc || !bc->row || !lv_obj_is_valid(bc->row)) {
        if (bc && bc->albums) free((void *)bc->albums);
        free(bc); lv_timer_del(timer); return;
    }
    /* Superseded by a newer refresh_list → abort this stale build. */
    if (bc->gen != bc->owner->build_gen) {
        free((void *)bc->albums); free(bc); lv_timer_del(timer); return;
    }
    int end = bc->next + CARDS_PER_TICK;
    if (end > bc->count) end = bc->count;
    for (int i = bc->next; i < end; i++) {
        lv_obj_t *card = lv_channel_card_create(bc->row, bc->albums[i]);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_SHORT_CLICKED, NULL);
    }
    bc->next = end;
    if (bc->next >= bc->count) {
        free((void *)bc->albums); free(bc); lv_timer_del(timer);
    }
}

/* ---- 分类 tab + 专辑卡片 ---- */
static void refresh_list(LocalPageCtx* ctx, channel_category_t cat) {
    for (int i = 0; i < CHANNEL_CATEGORY_COUNT; i++) {
        if (ctx->tab_pages[i]) lv_obj_clean(ctx->tab_pages[i]);
    }
    lv_obj_t* page = ctx->tab_pages[cat];
    if (!page) return;

    lv_obj_set_style_pad_top(page, CARD_PAD, 0);
    lv_obj_set_style_pad_left(page, 8, 0);
    lv_obj_set_style_pad_row(page, CARD_PAD, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    int count = 0;
    const Channel** albums = podcast_model_get_downloaded_channels_by_category(&g_podcast_app, cat, &count);
    if (count == 0) return;

    ctx->build_gen++;   /* invalidate any in-flight chunk build for this page */
    card_build_ctx_t *bc = (card_build_ctx_t *)calloc(1, sizeof(card_build_ctx_t));
    bc->row = page; bc->albums = albums; bc->count = count; bc->card_h = 0;
    bc->owner = ctx; bc->gen = ctx->build_gen;
    lv_timer_create(build_card_chunk_cb, 1, bc);
}

static void on_tab_changed(lv_event_t* e) {
    LocalPageCtx* ctx = lv_event_get_user_data(e);
    int active = lv_tabview_get_tab_active(lv_event_get_current_target_obj(e));
    if (active >= 0 && active < CHANNEL_CATEGORY_COUNT) refresh_list(ctx, (channel_category_t)active);
}

/* ---- Download-complete → live refresh (only when viewing Local) ---- */
static void on_download_complete_event(app_event_t event, const void *data) {
    (void)data;
    if (event != APP_EVENT_DOWNLOAD_COMPLETED) return;
    if (!s_local_screen || !lv_obj_is_valid(s_local_screen)) return;  /* page gone */
    if (s_local_screen != lv_screen_active()) return;                 /* not viewing Local now */

    if (s_local_ctx) {
        /* Content mode → refresh the currently active tab in place (keeps tab + scroll). */
        int active = lv_tabview_get_tab_active(s_local_ctx->tabview);
        if (active < 0 || active >= CHANNEL_CATEGORY_COUNT) active = 0;
        refresh_list(s_local_ctx, (channel_category_t)active);
    } else {
        /* Hint mode ("No audio downloaded yet") → content just became available;
         * rebuild so the tabview/cards appear. */
        page_navigator_navigate_to(&g_podcast_app.view->page_nav,
                                   &g_podcast_app, PAGE_LOCAL, NULL);
    }
}

/* ---- 页面构建 ---- */
static lv_obj_t* build_local_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;
    Page page = lv_page_create(NULL, false, NULL, NULL);
    /* Derive the content height from the display, NOT from lv_obj_get_height():
     * right after lv_page_create the container hasn't been laid out yet, so
     * get_height() returns 0 → the old `get_height() - TAB_BAR_HEIGHT` collapsed
     * the container to ~0px and the entire content area (tabs/cards/hint) was
     * invisible ("完全空白"). This page has no header, so only subtract the
     * status-bar spacer and the bottom tab bar. */
    int container_h = lv_display_get_vertical_resolution(lv_display_get_default())
                      - LV_STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT;
    lv_obj_set_height(page.container, container_h);

    if (!s_evt_registered) {
        app_event_register(on_download_complete_event);
        s_evt_registered = true;
    }
    s_local_ctx = NULL;   /* set below only in content mode */

    /* controller 检查并写入 model, view 从 model 读取 */
    podcast_controller_check_local_content(app);

    if (!podcast_model_is_local_sd_mounted(app)) {
        build_hint(page.container, "No SD card inserted, no local content.", false);
        printf("[INF] Local page built (no SD)\n"); fflush(stdout);
    } else if (!podcast_model_has_local_content(app)) {
        build_hint(page.container, "No audio downloaded yet. Please go to Network to download.", true);
        printf("[INF] Local page built (no content)\n"); fflush(stdout);
    } else {
        if (app->view->page_nav.nav_ctx) free(app->view->page_nav.nav_ctx);
        LocalPageCtx* ctx = malloc(sizeof(LocalPageCtx));
        memset(ctx, 0, sizeof(LocalPageCtx));
        app->view->page_nav.nav_ctx = ctx;
        s_local_ctx = ctx;   /* content mode → enable in-place live refresh */

        lv_obj_t* tv = lv_tabview_create(page.container);
        lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_grow(tv, 1);
        for (int i = 0; i < CHANNEL_CATEGORY_COUNT; i++) {
            ctx->tab_pages[i] = lv_tabview_add_tab(tv, CAT_NAMES[i]);
        }
        ctx->tabview = tv;
        lv_obj_add_event_cb(tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, ctx);

        lv_obj_t* tab_bar = lv_tabview_get_tab_bar(tv);
        lv_obj_set_style_pad_all(tab_bar, 0, 0);
        lv_obj_set_style_border_width(tab_bar, 0, 0);
        lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_height(tab_bar, 32);
        lv_obj_set_scroll_dir(tab_bar, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(tab_bar, LV_SCROLLBAR_MODE_ON);
        uint32_t n = lv_obj_get_child_count(tab_bar);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* tb = lv_obj_get_child(tab_bar, i);
            lv_obj_set_width(tb, 96);
            lv_obj_set_style_pad_hor(tb, 2, 0);
            lv_obj_set_style_flex_grow(tb, 0, 0);
            lv_obj_t* lbl = lv_obj_get_child(tb, 0);
            if (lbl) lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
        }

        refresh_list(ctx, CHANNEL_CATEGORY_NEWS_SOCIETY);
        printf("[INF] Local page built (content)\n"); fflush(stdout);
    }

    create_search_fab(page.screen);
    podcast_view_create_bottom_tab_bar(page.screen, TAB_LOCAL);
    s_local_screen = page.screen;
    return page.screen;
}

void podcast_view_local_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_LOCAL, build_local_page);
}
