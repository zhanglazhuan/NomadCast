/**
 * @file view_network.c
 * @brief 网络页 — 异步加载网络内容，Loading 动画 → 内容/异常/离线
 *
 * 流程: 打开页面 → Loading 动效 → 定时器轮询 model
 *       → 数据就绪: 展示内容
 *       → 超时: 展示异常 + 重试按钮
 *       → 离线: 展示离线提示 + 本地链接
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view_network.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "../hal.h"
#include "lv_page.h"
#include "../lv_channel_card.h"
#include "launcher.h"
#include "app_manager.h"

extern PodcastApp g_podcast_app;
extern const lv_image_dsc_t ic_search;
extern const lv_image_dsc_t ic_forward_media;

/* 加载超时 (ms), 方便后续修改 */
#define NETWORK_FETCH_TIMEOUT_MS 30000
/* Loading 动画刷新间隔 (ms) */
#define LOADING_ANIM_INTERVAL_MS 400

static const char* CATEGORY_NAMES[CHANNEL_CATEGORY_COUNT] = {
    "时事", "科技", "人文", "生活", "教育", "其他",
};

typedef struct {
    lv_obj_t* tabview;
    lv_obj_t* tab_pages[CHANNEL_CATEGORY_COUNT];
} NetworkPageCtx;

/* ── 页面状态 ─────────────────────────────────────────────────────────── */
typedef enum {
    NP_STATE_LOADING,   // Loading 动效中
    NP_STATE_ONLINE,    // 内容就绪
    NP_STATE_OFFLINE,   // 无网络
    NP_STATE_ERROR,     // 后端异常
} NetworkPageState;

typedef struct {
    NetworkPageState state;
    lv_obj_t*        container;       // 父容器
    lv_obj_t*        loading_label;   // "Loading." 文本
    lv_timer_t*      anim_timer;      // 加载动画定时器
    lv_timer_t*      timeout_timer;   // 超时定时器
    lv_timer_t*      poll_timer;      // 轮询 model 定时器
    NetworkPageCtx*  ctx;             // 在线内容上下文
    bool              content_built;  // 是否已构建内容
    int               dot_count;      // 加载动画点号计数
    int               dot_dir;        // 加载动画方向
} NetworkPage;

/* 前向声明 — timer 回调中引用 */
static void stop_all_timers(NetworkPage* np);
static lv_obj_t* build_loading(lv_obj_t* parent);
static void build_online_content(NetworkPageCtx* ctx, lv_obj_t* parent);
static void build_error(lv_obj_t* parent, const char* msg, NetworkPage* np);
static void on_card_clicked(lv_event_t* e);

/* ── Fetch trigger (one-shot timer) — calls blocking HTTP fetch ───────── */

static void fetch_chart_trigger_cb(lv_timer_t* timer) {
    (void)timer;
    printf("[INF] Network page: starting chart fetch...\n"); fflush(stdout);
    podcast_controller_fetch_chart(&g_podcast_app);
    lv_timer_del(timer);
}

/* ── WiFi offline UI — "No network" + Open Settings button ─────────────── */

static void deferred_open_settings(lv_timer_t* timer) {
    launcher_open_app("Settings");
    lv_timer_del(timer);
}

static void on_open_settings_clicked(lv_event_t* e) {
    (void)e;
    /* Close Podcast app, then open Settings (deferred to avoid use-after-free) */
    app_manager_delete();
    lv_timer_create(deferred_open_settings, 300, NULL);
}

static void build_wifi_offline(lv_obj_t* parent, NetworkPage* np) {
    (void)np;
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "No network.\nPlease connect WiFi in Settings.");
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label, g_cjk_font, 0);
    lv_obj_set_size(label, 200, 48);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);

    /* Open Settings button */
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn, 160, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_open_settings_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 24);

    lv_obj_t* btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Open Settings");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(btn_label, g_cjk_font, 0);
    lv_obj_center(btn_label);
}

static void on_page_delete(lv_event_t* e) {
    NetworkPage* np = lv_event_get_user_data(e);
    if (!np) return;
    stop_all_timers(np);
    printf("[INF] Network page timers stopped\n"); fflush(stdout);
}

/* 页面级异步回调: 模拟后端延迟返回 */
static void stop_all_timers(NetworkPage* np) {
    if (np->anim_timer)    { lv_timer_del(np->anim_timer);    np->anim_timer = NULL; }
    if (np->timeout_timer) { lv_timer_del(np->timeout_timer); np->timeout_timer = NULL; }
    if (np->poll_timer)    { lv_timer_del(np->poll_timer);    np->poll_timer = NULL; }
}

/* ── 加载动画: 点号从 1→3→1 循环 ─────────────────────────────────────── */

static void loading_anim_cb(lv_timer_t* timer) {
    NetworkPage* np = lv_timer_get_user_data(timer);
    if (!np || !np->loading_label) return;

    np->dot_count += np->dot_dir;
    if (np->dot_count >= 3) { np->dot_count = 3; np->dot_dir = -1; }
    if (np->dot_count <= 1) { np->dot_count = 1; np->dot_dir =  1; }

    char buf[16];
    snprintf(buf, sizeof(buf), "Loading%.*s", np->dot_count, "...");
    lv_label_set_text(np->loading_label, buf);
}

/* ── 超时 / 轮询 检查 ─────────────────────────────────────────────────── */

static void fetch_timeout_cb(lv_timer_t* timer) {
    NetworkPage* np = lv_timer_get_user_data(timer);
    if (!np || np->content_built) return;

    net_state_t st = podcast_model_get_net_state(&g_podcast_app);
    if (st == NET_STATE_READY) return;

    /* Timeout → show error */
    np->state = NP_STATE_ERROR;
    stop_all_timers(np);
    lv_obj_clean(np->container);
    build_error(np->container, "Request timed out. Please retry.", np);
    np->content_built = true;
}

/* 轮询检查 model 状态 + WiFi 重连检测 */
static void poll_ready_cb(lv_timer_t* timer) {
    NetworkPage* np = lv_timer_get_user_data(timer);
    if (!np || np->content_built) {
        lv_timer_del(timer);
        if (np) np->poll_timer = NULL;  /* prevent double-free in stop_all_timers */
        return;
    }

    net_state_t st = podcast_model_get_net_state(&g_podcast_app);

    if (st == NET_STATE_READY) {
        stop_all_timers(np);

        /* 数据就绪 → 构建内容 */
        lv_obj_clean(np->container);
        NetworkPageCtx* ctx = malloc(sizeof(NetworkPageCtx));
        memset(ctx, 0, sizeof(NetworkPageCtx));
        if (g_podcast_app.view->page_nav.nav_ctx)
            free(g_podcast_app.view->page_nav.nav_ctx);
        g_podcast_app.view->page_nav.nav_ctx = ctx;
        build_online_content(ctx, np->container);
        np->content_built = true;
        printf("[INF] Network page built (online) container=%p screen=%p active_screen=%p\n",
               (void*)np->container, (void*)lv_obj_get_screen(np->container),
               (void*)lv_screen_active()); fflush(stdout);
        return;
    }

    if (st == NET_STATE_ERROR && np->state == NP_STATE_LOADING) {
        /* Fetch completed with error */
        stop_all_timers(np);
        np->state = NP_STATE_ERROR;
        lv_obj_clean(np->container);
        const char* err = podcast_model_get_net_error(&g_podcast_app);
        build_error(np->container, err ? err : "Failed to load content", np);
        np->content_built = true;
        printf("[INF] Network page built (error)\n"); fflush(stdout);
        return;
    }

    /* WiFi reconnection detection: user went to Settings, connected WiFi, came back */
    if ((np->state == NP_STATE_OFFLINE || np->state == NP_STATE_ERROR)
        && hal_wifi_is_connected()) {
        stop_all_timers(np);
        np->state = NP_STATE_LOADING;
        lv_obj_clean(np->container);
        np->loading_label = build_loading(np->container);
        np->dot_count = 1; np->dot_dir = 1;
        np->anim_timer    = lv_timer_create(loading_anim_cb, LOADING_ANIM_INTERVAL_MS, np);
        np->timeout_timer = lv_timer_create(fetch_timeout_cb, NETWORK_FETCH_TIMEOUT_MS, np);
        np->poll_timer    = lv_timer_create(poll_ready_cb, 200, np);
        lv_timer_create(fetch_chart_trigger_cb, 100, np);
        printf("[INF] Network page: WiFi reconnected, fetching chart\n"); fflush(stdout);
    }
}

/* ── Loading 视图 ─────────────────────────────────────────────────────── */

static lv_obj_t* build_loading(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "Loading.");
    lv_obj_set_style_text_color(label, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(label, g_cjk_font, 0);
    lv_obj_set_size(label, 160, 24);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    return label;
}

/* ── Chunked card creation to avoid watchdog ──────────────────────────────── */

#define CARDS_PER_TICK 2  /* create this many cards per LVGL tick */

typedef struct {
    lv_obj_t      *row;
    const Channel **albums;
    int             count;
    int             card_h;
    int             next;     /* next index to create */
} card_build_ctx_t;

static void build_card_chunk_cb(lv_timer_t *timer) {
    card_build_ctx_t *bc = (card_build_ctx_t *)lv_timer_get_user_data(timer);
    if (!bc || !bc->row || !lv_obj_is_valid(bc->row)) {
        if (bc && bc->albums) free((void *)bc->albums);
        free(bc);
        lv_timer_del(timer);
        return;
    }

    int end = bc->next + CARDS_PER_TICK;
    if (end > bc->count) end = bc->count;

    for (int i = bc->next; i < end; i++) {
        lv_obj_t *card = lv_channel_card_create(bc->row, bc->albums[i]);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_SHORT_CLICKED, NULL);
    }
    bc->next = end;

    if (bc->next >= bc->count) {
        free((void *)bc->albums);
        free(bc);
        lv_timer_del(timer);
    }
}

static void refresh_list(NetworkPageCtx* ctx, channel_category_t cat) {
    lv_obj_t* page = ctx->tab_pages[cat];
    if (!page) { printf("[refresh] no page for cat=%d\n", cat); fflush(stdout); return; }

    /* Clear, add scroll container */
    lv_obj_clean(page);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF5F5F5), 0);

    int offset = podcast_model_get_network_shown(&g_podcast_app, cat);
    int count = 0;
    const Channel** albums = podcast_model_get_network_page(&g_podcast_app, cat, offset, NETWORK_PAGE_SIZE, &count);

    /* Clear, rebuild as vertical scrolling list */
    lv_obj_clean(page);
    lv_obj_set_style_pad_top(page, CARD_PAD, 0);
    lv_obj_set_style_pad_left(page, 8, 0);
    lv_obj_set_style_pad_row(page, CARD_PAD, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    printf("[refresh] cat=%d count=%d offset=%d\n", cat, count, offset); fflush(stdout);

    if (count == 0) return;

    /* Chunked card creation */
    card_build_ctx_t *bc = (card_build_ctx_t *)calloc(1, sizeof(card_build_ctx_t));
    bc->row    = page;
    bc->albums = albums;
    bc->count  = count;
    bc->card_h = 0; /* unused now */
    lv_timer_create(build_card_chunk_cb, 1, bc);
}

/* 滚动到末尾时触发加载更多 */
static void on_tab_changed(lv_event_t* e) {
    NetworkPageCtx* ctx = lv_event_get_user_data(e);
    int active = lv_tabview_get_tab_active(lv_event_get_current_target_obj(e));
    if (active >= 0 && active < CHANNEL_CATEGORY_COUNT) {
        podcast_model_set_network_active_category(&g_podcast_app, active);
        refresh_list(ctx, (channel_category_t)active);
    }
}

static void build_online_content(NetworkPageCtx* ctx, lv_obj_t* parent) {
    lv_obj_t* tv = lv_tabview_create(parent);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(tv, 1);
    for (int i = 0; i < CHANNEL_CATEGORY_COUNT; i++) {
        ctx->tab_pages[i] = lv_tabview_add_tab(tv, CATEGORY_NAMES[i]);
        lv_obj_set_user_data(ctx->tab_pages[i], (void*)(uintptr_t)i);
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
        if (lbl) {
            lv_obj_set_style_text_font(lbl, g_cjk_font, 0);
        }
    }

    int cat = podcast_model_get_network_active_category(&g_podcast_app);
    lv_tabview_set_active(ctx->tabview, cat, LV_ANIM_OFF);

    /* Ensure content is loaded — set_active only fires VALUE_CHANGED if
     * the tab actually changed.  If the stored category is the default (0),
     * the event won't fire; call refresh_list explicitly. */
    if (ctx->tab_pages[cat] && lv_obj_get_child_count(ctx->tab_pages[cat]) == 0) {
        refresh_list(ctx, (channel_category_t)cat);
    }

    /* Scroll tab bar so the active tab is visible */
    tab_bar = lv_tabview_get_tab_bar(ctx->tabview);
    uint32_t active_idx = (uint32_t)cat;
    if (active_idx < lv_obj_get_child_count(tab_bar)) {
        lv_obj_scroll_to_view(lv_obj_get_child(tab_bar, active_idx), LV_ANIM_OFF);
    }
}

/* ── 搜索 / 本地链接 ───────────────────────────────────────────────────── */

static void on_forward_clicked(lv_event_t* e) {
    (void)e;
    NetworkPageCtx* ctx = g_podcast_app.view->page_nav.nav_ctx;
    if (!ctx || !ctx->tabview) return;
    int active = lv_tabview_get_tab_active(ctx->tabview);
    if (active < 0 || active >= CHANNEL_CATEGORY_COUNT) return;

    channel_category_t cat = (channel_category_t)active;
    int total = podcast_model_get_network_total(&g_podcast_app, cat);
    int shown = podcast_model_get_network_shown(&g_podcast_app, cat);

    /* Advance offset by NETWORK_PAGE_SIZE, wrap to 0 when at end */
    int offset = shown + NETWORK_PAGE_SIZE;
    if (offset >= total) offset = 0;
    g_podcast_app.model->network_channels_shown[cat] = offset;

    fprintf(stderr, "[FWD] cat=%d total=%d offset=%d\n", cat, total, offset);
    refresh_list(ctx, cat);
}

static void on_search_clicked(lv_event_t* e) {
    (void)e;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_NETWORK, PAGE_SEARCH, NULL);
}

/* ── 点击处理 ────────────────────────────────────────────────────────── */

static void on_card_clicked(lv_event_t* e) {
    lv_obj_t* card = lv_event_get_current_target_obj(e);
    int channel_id = (int)(uintptr_t)lv_obj_get_user_data(card);
    printf("[INF] Card clicked, channel_id=%d\n", channel_id); fflush(stdout);
    int* id_ptr = malloc(sizeof(int));
    *id_ptr = channel_id;
    PAGE_NAVIGATE_TO((&g_podcast_app), PAGE_NETWORK, PAGE_CHANNEL, id_ptr);
}

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

static void create_forward_fab(lv_obj_t* screen) {
    lv_obj_t* fab = lv_button_create(screen);
    lv_obj_set_size(fab, FAB_SIZE, FAB_SIZE);
    lv_obj_set_style_radius(fab, FAB_SIZE / 2, 0);
    lv_obj_set_style_bg_color(fab, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(fab, 0, 0);
    lv_obj_set_style_shadow_width(fab, 20, 0);
    lv_obj_set_style_shadow_color(fab, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(fab, LV_OPA_30, 0);
    lv_obj_add_flag(fab, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(fab, on_forward_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -FAB_MARGIN_RIGHT, -96);

    lv_obj_t* img = lv_image_create(fab);
    lv_image_set_src(img, &ic_forward_media);
    lv_obj_center(img);
    lv_obj_set_style_img_recolor(img, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
}

/* ── 异常页: 错误信息 + 重试按钮 ──────────────────────────────────────── */

static void on_retry_clicked(lv_event_t* e) {
    NetworkPage* np = lv_event_get_user_data(e);
    if (!hal_wifi_is_connected()) {
        /* WiFi still not connected → show offline */
        lv_obj_clean(np->container);
        np->state = NP_STATE_OFFLINE;
        build_wifi_offline(np->container, np);
        np->content_built = true;
        np->poll_timer = lv_timer_create(poll_ready_cb, 2000, np);
        return;
    }

    /* Reset loading state + trigger fetch */
    lv_obj_clean(np->container);
    np->state = NP_STATE_LOADING;
    np->content_built = false;
    np->loading_label = build_loading(np->container);
    podcast_model_set_net_state(&g_podcast_app, NET_STATE_IDLE, NULL);

    /* One-shot fetch timer */
    lv_timer_create(fetch_chart_trigger_cb, 100, np);

    /* Animation + timeout + poll */
    np->dot_count = 1; np->dot_dir = 1;
    np->anim_timer    = lv_timer_create(loading_anim_cb, LOADING_ANIM_INTERVAL_MS, np);
    np->timeout_timer = lv_timer_create(fetch_timeout_cb, NETWORK_FETCH_TIMEOUT_MS, np);
    np->poll_timer    = lv_timer_create(poll_ready_cb, 200, np);
}

static void build_error(lv_obj_t* parent, const char* msg, NetworkPage* np) {
    lv_obj_t* line1 = lv_label_create(parent);
    lv_label_set_text(line1, msg ? msg : "Failed to load content");
    lv_obj_set_style_text_color(line1, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(line1, g_cjk_font, 0);
    lv_obj_set_size(line1, 200, 24);
    lv_obj_set_style_text_align(line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(line1, LV_ALIGN_CENTER, 0, -30);

    /* Retry button — centered below, floating */
    lv_obj_t* retry_btn = lv_button_create(parent);
    lv_obj_add_flag(retry_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(retry_btn, 120, 40);
    lv_obj_set_style_bg_color(retry_btn, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(retry_btn, 0, 0);
    lv_obj_set_style_radius(retry_btn, 8, 0);
    lv_obj_add_event_cb(retry_btn, on_retry_clicked, LV_EVENT_CLICKED, np);
    lv_obj_align(retry_btn, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t* retry_label = lv_label_create(retry_btn);
    lv_label_set_text(retry_label, "Retry");
    lv_obj_center(retry_label);
    lv_obj_set_style_text_color(retry_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(retry_label, g_cjk_font, 0);
}

/* ── 页面构建 ─────────────────────────────────────────────────────────── */

static lv_obj_t* build_network_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;
    Page page = lv_page_create(NULL, false, NULL, NULL);
    /* Reserve space for bottom tab bar (now floating).
     * Compute height arithmetically — lv_obj_get_height may return 0
     * before LVGL has performed its first layout pass. */
    int container_h = lv_display_get_vertical_resolution(lv_display_get_default())
                      - LV_STATUS_BAR_HEIGHT
                      - TAB_BAR_HEIGHT;
    lv_obj_set_height(page.container, container_h);

    /* 分配页面上下文 (生命周期同页面) */
    NetworkPage* np = malloc(sizeof(NetworkPage));
    memset(np, 0, sizeof(NetworkPage));
    np->container = page.container;

    /* ── WiFi check first — gate all network operations ── */
    if (!hal_wifi_is_connected()) {
        /* ====== No WiFi → offline hint + Open Settings button ====== */
        np->state = NP_STATE_OFFLINE;
        build_wifi_offline(page.container, np);
        np->content_built = true;
        /* Start a slow poll to detect WiFi reconnection */
        np->poll_timer = lv_timer_create(poll_ready_cb, 2000, np);
        printf("[INF] Network page built (no WiFi)\n"); fflush(stdout);
    } else {
        /* WiFi is connected — check if data already loaded */
        net_state_t st = podcast_model_get_net_state(app);
        if (st == NET_STATE_READY) {
            /* ====== Data cached → show content directly ====== */
            np->state = NP_STATE_ONLINE;
            NetworkPageCtx* ctx = malloc(sizeof(NetworkPageCtx));
            memset(ctx, 0, sizeof(NetworkPageCtx));
            if (g_podcast_app.view->page_nav.nav_ctx)
                free(g_podcast_app.view->page_nav.nav_ctx);
            g_podcast_app.view->page_nav.nav_ctx = ctx;
            build_online_content(ctx, np->container);
            np->content_built = true;
            printf("[INF] Network page built (cached)\n"); fflush(stdout);
        } else {
            /* ====== WiFi OK, no data → Loading + trigger fetch ====== */
            np->state = NP_STATE_LOADING;
            np->loading_label = build_loading(page.container);

            /* One-shot timer triggers the blocking HTTP fetch (lets UI render first) */
            lv_timer_create(fetch_chart_trigger_cb, 100, np);

            /* Animation + timeout + poll timers */
            np->dot_count = 1; np->dot_dir = 1;
            np->anim_timer    = lv_timer_create(loading_anim_cb, LOADING_ANIM_INTERVAL_MS, np);
            np->timeout_timer = lv_timer_create(fetch_timeout_cb, NETWORK_FETCH_TIMEOUT_MS, np);
            np->poll_timer    = lv_timer_create(poll_ready_cb, 200, np);

            printf("[INF] Network page built (loading, WiFi OK)\n"); fflush(stdout);
        }
    }

    /* 悬浮搜索 + 底栏 (所有状态均显示) */
    create_search_fab(page.screen);
    create_forward_fab(page.screen);
    podcast_view_create_bottom_tab_bar(page.screen, TAB_NETWORK);

    /* 页面销毁时清理定时器，防止切换 tab 崩溃 */
    lv_obj_add_event_cb(page.screen, on_page_delete, LV_EVENT_DELETE, np);

    return page.screen;
}

void podcast_view_network_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_NETWORK, build_network_page);
}
