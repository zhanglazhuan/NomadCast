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
#include "lv_bottom_sheet.h"

extern PodcastApp g_podcast_app;
extern const lv_image_dsc_t ic_search;

#define CAT_ALL  (-1)

typedef struct {
    lv_obj_t* dropdown;
    lv_obj_t* content;
    int       active_cat;
    uint32_t  build_gen;
} LocalPageCtx;

/* Live Local page tracking for download-complete refresh.
 * The event bus has no unregister, so we register once and validate liveness. */
static lv_obj_t*     s_local_screen   = NULL;  /* live Local screen, or NULL */
static LocalPageCtx* s_local_ctx      = NULL;  /* set in content mode; NULL in hint mode */
static bool          s_evt_registered = false;

static void on_search_bar_clicked(lv_event_t* e) {
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

/* ── 左滑删除 ────────────────────────────────────────────────────────── */

static lv_bottom_sheet_t *s_delete_sheet = NULL;
static int                s_delete_channel_id = 0;

static void on_delete_sheet_destroy(lv_event_t *e)
{
    (void)e;
    s_delete_sheet = NULL;
}

static void on_delete_confirm(lv_event_t *e)
{
    (void)e;
    if (s_delete_sheet) {
        lv_bottom_sheet_close(s_delete_sheet);
        s_delete_sheet = NULL;
    }
    podcast_controller_delete_channel_local(&g_podcast_app, s_delete_channel_id);

    /* Refresh the Local page to reflect the deletion */
    page_navigator_navigate_to(&g_podcast_app.view->page_nav,
                               &g_podcast_app, PAGE_LOCAL, NULL);
}

static void show_delete_sheet(const Channel *ch)
{
    if (s_delete_sheet || !ch) return;
    s_delete_channel_id = ch->id;

    s_delete_sheet = lv_bottom_sheet_create(lv_screen_active());
    lv_obj_add_event_cb(s_delete_sheet->overlay, on_delete_sheet_destroy,
                        LV_EVENT_DELETE, NULL);

    lv_obj_t *content = lv_bottom_sheet_get_content(s_delete_sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 16, 0);
    lv_obj_set_style_pad_row(content, 12, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text_fmt(title, "Delete \"%s\"?", ch->title);
    lv_obj_set_style_text_font(title, g_cjk_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(content);
    lv_label_set_text(sub, "This will remove all downloaded\naudio files for this channel.");
    lv_obj_set_style_text_font(sub, g_cjk_font, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x999999), 0);

    /* Delete button */
    lv_obj_t *btn = lv_button_create(content);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_delete_confirm, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Delete");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(btn_lbl, g_cjk_font, 0);
    lv_obj_center(btn_lbl);
}

static void on_card_swipe(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT) return;

    lv_obj_t *card = lv_event_get_current_target_obj(e);
    int channel_id = (int)(uintptr_t)lv_obj_get_user_data(card);
    const Channel *ch = podcast_model_get_channel_by_id(&g_podcast_app, channel_id);
    if (ch) show_delete_sheet(ch);
}

/* ---- "Network" 链接点击 ---- */
static void on_network_link_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_LOCAL, PAGE_NETWORK, NULL);
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
        lv_obj_add_event_cb(card, on_card_swipe, LV_EVENT_GESTURE, NULL);
    }
    bc->next = end;
    if (bc->next >= bc->count) {
        free((void *)bc->albums); free(bc); lv_timer_del(timer);
    }
}

/* ---- Dropdown + card list ---- */
static void refresh_list(LocalPageCtx* ctx, int cat) {
    lv_obj_t* page = ctx->content;
    if (!page) return;
    lv_obj_clean(page);

    int total = 0;
    const Channel **albums = NULL;

    if (cat == CAT_ALL) {
        for (int c = 0; c < CHANNEL_CATEGORY_COUNT; c++) {
            int n = 0;
            const Channel **a = podcast_model_get_downloaded_channels_by_category(
                &g_podcast_app, (channel_category_t)c, &n);
            if (n > 0) {
                albums = (const Channel **)realloc(albums,
                            (total + n) * sizeof(Channel *));
                memcpy(albums + total, a, n * sizeof(Channel *));
                total += n;
            }
        }
    } else {
        albums = podcast_model_get_downloaded_channels_by_category(
            &g_podcast_app, (channel_category_t)cat, &total);
    }

    lv_obj_set_style_pad_top(page, CARD_PAD, 0);
    lv_obj_set_style_pad_left(page, 8, 0);
    lv_obj_set_style_pad_row(page, CARD_PAD, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    if (total == 0) { free(albums); return; }

    ctx->build_gen++;
    card_build_ctx_t *bc = (card_build_ctx_t *)calloc(1, sizeof(card_build_ctx_t));
    bc->row = page; bc->albums = albums; bc->count = total;
    bc->owner = ctx; bc->gen = ctx->build_gen;
    lv_timer_create(build_card_chunk_cb, 1, bc);
}

static void on_dropdown_changed(lv_event_t* e) {
    LocalPageCtx* ctx = lv_event_get_user_data(e);
    uint32_t sel = lv_dropdown_get_selected(lv_event_get_current_target_obj(e));
    ctx->active_cat = (sel == 0) ? CAT_ALL : (int)(sel - 1);
    refresh_list(ctx, ctx->active_cat);
}

/* ---- Download-complete → live refresh (only when viewing Local) ---- */
static void on_download_complete_event(app_event_t event, const void *data) {
    (void)data;
    if (event != APP_EVENT_DOWNLOAD_COMPLETED) return;
    if (!s_local_screen || !lv_obj_is_valid(s_local_screen)) return;  /* page gone */
    if (s_local_screen != lv_screen_active()) return;                 /* not viewing Local now */

    if (s_local_ctx) {
        refresh_list(s_local_ctx, s_local_ctx->active_cat);
    } else {
        /* Hint mode → content just became available; rebuild the page. */
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
        s_local_ctx = ctx;

        /* Top bar: [search flex] --gap-- [dropdown 35%] = 95% wide */
        lv_obj_t *top_bar = lv_obj_create(page.container);
        lv_obj_set_size(top_bar, LV_PCT(95), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(top_bar, 0, 0);
        lv_obj_set_style_pad_left(top_bar, 8, 0);
        lv_obj_set_style_pad_column(top_bar, 6, 0);
        lv_obj_set_style_border_width(top_bar, 0, 0);
        lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *search_btn = lv_button_create(top_bar);
        lv_obj_set_flex_grow(search_btn, 1);
        lv_obj_set_height(search_btn, 32);
        lv_obj_set_style_bg_color(search_btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(search_btn, 2, 0);
        lv_obj_set_style_border_color(search_btn, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_border_opa(search_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(search_btn, 6, 0);
        lv_obj_set_style_shadow_width(search_btn, 0, 0);
        lv_obj_add_event_cb(search_btn, on_search_bar_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_t *search_lbl = lv_label_create(search_btn);
        lv_label_set_text(search_lbl, "Search...");
        lv_obj_set_style_text_color(search_lbl, lv_color_hex(0x999999), 0);
        lv_obj_align(search_lbl, LV_ALIGN_LEFT_MID, 8, 0);

        ctx->dropdown = lv_dropdown_create(top_bar);
        lv_dropdown_set_options(ctx->dropdown,
            "全部\n时事\n科技\n人文\n生活\n教育\n其他");
        lv_obj_set_width(ctx->dropdown, LV_PCT(35));
        lv_obj_set_height(ctx->dropdown, 32);
        lv_obj_set_style_border_width(ctx->dropdown, 2, 0);
        lv_obj_set_style_border_color(ctx->dropdown, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_border_opa(ctx->dropdown, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ctx->dropdown, 6, 0);
        lv_obj_set_style_pad_hor(ctx->dropdown, 8, 0);
        lv_obj_add_event_cb(ctx->dropdown, on_dropdown_changed,
                            LV_EVENT_VALUE_CHANGED, ctx);

        lv_obj_t *dd_list = lv_dropdown_get_list(ctx->dropdown);
        lv_obj_set_width(dd_list, lv_pct(55));
        lv_obj_set_style_pad_hor(dd_list, 6, 0);

        /* Content */
        ctx->content = lv_obj_create(page.container);
        lv_obj_set_size(ctx->content, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_grow(ctx->content, 1);

        ctx->active_cat = CAT_ALL;
        lv_dropdown_set_selected(ctx->dropdown, 0);
        refresh_list(ctx, CAT_ALL);
        printf("[INF] Local page built (content)\n"); fflush(stdout);
    }

    podcast_view_create_bottom_tab_bar(page.screen, TAB_LOCAL);
    s_local_screen = page.screen;
    return page.screen;
}

void podcast_view_local_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_LOCAL, build_local_page);
}
