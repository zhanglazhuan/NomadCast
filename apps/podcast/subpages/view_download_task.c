/**
 * @file view_download_task.c
 * @brief 下载任务页 — 统计卡片 + 表头(全选switch + 选中数) + 音频列表 + 批量操作
 *
 * 参照 view_album.c 的曲目列表模式：上下文结构体管理 checkbox 数组和选中计数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "view_download_task.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "app_event.h"
#include "lv_page.h"
#include "lv_bottom_sheet.h"

extern PodcastApp g_podcast_app;

/* ---- 页面上下文 ---- */
#define DL_PER_PAGE 20

typedef struct DownloadTaskPageCtx_s {
    struct PodcastApp* app;
    lv_obj_t** task_cbs;
    bool*     task_checked;
    int       task_count;
    lv_obj_t* sel_label;
    lv_obj_t* action_bar;
    lv_obj_t* pause_btn;
    lv_obj_t* pause_label;
    lv_obj_t* delete_btn;
    lv_bottom_sheet_t* confirm_sheet;
    lv_obj_t* list_container;
    int       cur_page;
    int       total_pages;
    lv_obj_t* page_label;
    lv_obj_t* prev_btn;
    lv_obj_t* next_btn;
} DownloadTaskPageCtx;

static DownloadTaskPageCtx *s_active_dl_page = NULL;

static void dl_prev_page(lv_event_t *e);
static void dl_next_page(lv_event_t *e);
static lv_obj_t* build_task_list(lv_obj_t* parent, DownloadTaskPageCtx* ctx);

/* ---- Download-complete event → refresh page via navigator ---- */

static void on_download_complete_event(app_event_t event, const void *data)
{
    (void)data;
    /* COMPLETED (success) and CHANGED (e.g. failure) both need a fresh stats
     * card + list, otherwise a failed task leaves the counts on a stale
     * "downloading" snapshot. */
    if (event != APP_EVENT_DOWNLOAD_COMPLETED &&
        event != APP_EVENT_DOWNLOAD_CHANGED) return;
    if (!s_active_dl_page) return;
    if (!s_active_dl_page->app) return;

    /* Full page rebuild via navigator — more robust than in-place list rebuild
     * because it recreates the screen, ensuring LVGL flex layout recalculates
     * correctly.  Don't push to back-stack (PAGE_DOWNLOAD_TASK is already the
     * current page), just navigate to it. */
    /* Validate the pointer still references a live screen before rebuilding.
     * (s_active_dl_page is only cleared at the start of the next page build,
     * never asynchronously, to avoid the lv_obj_delete_async race.) */
    if (!lv_obj_is_valid(s_active_dl_page->list_container)) return;

    PodcastApp *app = s_active_dl_page->app;
    page_navigator_navigate_to(&app->view->page_nav, app, PAGE_DOWNLOAD_TASK, NULL);
}

static void on_confirm_sheet_delete(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    if (ctx) ctx->confirm_sheet = NULL;
}

static void on_sheet_cancel(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    if (ctx && ctx->confirm_sheet) {
        lv_bottom_sheet_close(ctx->confirm_sheet);
        ctx->confirm_sheet = NULL;
    }
}

/* ---- 辅助: 预估耗时 (基于实测网速) ----
 * ETA = 剩余字节 / 实测平均吞吐。正在下载的任务用精确剩余字节；排队中的任务
 * 用 时长 × 实测"每音频秒字节数" 估算大小。首个采样到来前用保守默认值兜底。 */
static void calc_est_time(struct PodcastApp* app, char* buf, int buf_size) {
    int avg_bps = podcast_dl_avg_speed_bps();
    if (avg_bps <= 0) avg_bps = 200 * 1024;         /* ~200 KB/s 假定 WiFi 吞吐 */

    int bps_audio = podcast_dl_bytes_per_audio_sec();
    if (bps_audio <= 0) bps_audio = 16 * 1024;      /* ~128 kbps 回退码率 */

    int active_id = podcast_dl_current_task_id();

    int64_t remaining_bytes = 0;
    int task_count = 0;
    for (int i = 0; i < app->model->download_task_count; i++) {
        const DownloadTask* t = &app->model->download_tasks[i];
        if (t->status == DOWNLOAD_STATUS_DOWNLOADING && t->id == active_id) {
            int rem = podcast_dl_current_remaining_bytes();
            /* 服务器给了 Content-Length → 精确剩余；否则按剩余时长比例估算 */
            remaining_bytes += rem > 0
                ? rem
                : (int64_t)t->duration_sec * (100 - t->progress) / 100 * bps_audio;
            task_count++;
        } else if (t->status == DOWNLOAD_STATUS_DOWNLOADING) {
            remaining_bytes += (int64_t)t->duration_sec * (100 - t->progress) / 100 * bps_audio;
            task_count++;
        } else if (t->status == DOWNLOAD_STATUS_PENDING) {
            remaining_bytes += (int64_t)t->duration_sec * bps_audio;
            task_count++;
        }
    }
    if (task_count == 0) {
        snprintf(buf, buf_size, "--");
        return;
    }

    int est_sec = (int)(remaining_bytes / avg_bps);
    if (est_sec < 60) {
        snprintf(buf, buf_size, "~%d sec", est_sec);
    } else {
        snprintf(buf, buf_size, "~%d min", est_sec / 60);
    }
}

/* ---- 辅助: 统计各状态数量 ---- */
static void count_statuses(struct PodcastApp* app, int* pending, int* completed,
                           int* failed) {
    int downloading = 0;
    *pending = 0; *completed = 0; *failed = 0;
    for (int i = 0; i < app->model->download_task_count; i++) {
        switch (app->model->download_tasks[i].status) {
            case DOWNLOAD_STATUS_PENDING:     (*pending)++;     break;
            case DOWNLOAD_STATUS_DOWNLOADING: downloading++;    break;
            case DOWNLOAD_STATUS_COMPLETED:   (*completed)++;   break;
            case DOWNLOAD_STATUS_FAILED:      (*failed)++;      break;
            default: break;
        }
    }
    *pending += downloading;  /* "Pending" = queued tasks not yet finished */
}

/* ---- 全选 / 全不选 ---- */
static void set_all_checked(DownloadTaskPageCtx* ctx, bool val) {
    for (int i = 0; i < ctx->task_count; i++) {
        ctx->task_checked[i] = val;
        if (ctx->task_cbs[i]) {
            if (val) lv_obj_add_state(ctx->task_cbs[i], LV_STATE_CHECKED);
            else     lv_obj_remove_state(ctx->task_cbs[i], LV_STATE_CHECKED);
        }
    }
}

/* ---- 更新选中计数 & 操作栏可见性 ---- */
static void update_sel_label(DownloadTaskPageCtx* ctx) {
    int n = 0;
    for (int i = 0; i < ctx->task_count; i++)
        if (ctx->task_checked[i]) n++;

    /* 更新选中数标签 */
    if (ctx->sel_label) {
        lv_label_set_text_fmt(ctx->sel_label, "%d", n);
    }

    /* Delete 对任意选中项都生效（已完成→删记录留文件；下载中→取消并删半成品），
     * 故只要有选中就显示操作栏。 */
    if (n > 0) {
        lv_obj_clear_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- 全选 switch 回调 ---- */
static void on_select_all_switch(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* sw = lv_event_get_current_target_obj(e);
    set_all_checked(ctx, lv_obj_has_state(sw, LV_STATE_CHECKED));
    update_sel_label(ctx);
}

/* ---- checkbox 变更回调 ---- */
static void on_checkbox_changed(lv_event_t* e) {
    lv_obj_t* cb = lv_event_get_current_target_obj(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(cb);
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    ctx->task_checked[idx] = lv_obj_has_state(cb, LV_STATE_CHECKED);
    update_sel_label(ctx);
}

/* ---- 删除确认回调 ----
 * 已完成 → 删记录、保留音频文件；下载中/等待中 → 取消并删半成品文件。 */
static void on_delete_confirm(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    if (ctx && ctx->confirm_sheet) {
        lv_bottom_sheet_close(ctx->confirm_sheet);
        ctx->confirm_sheet = NULL;
    }
    int total = ctx->task_count;
    int* keep_file_ids = (int*)malloc(sizeof(int) * total);  /* 删记录、留文件 */
    int* purge_ids     = (int*)malloc(sizeof(int) * total);  /* 取消、删半成品 */
    int nk = 0, np = 0;
    for (int i = 0; i < total; i++) {
        if (!ctx->task_checked[i]) continue;
        download_status_t s = ctx->app->model->download_tasks[i].status;
        int id = ctx->app->model->download_tasks[i].id;
        if (s == DOWNLOAD_STATUS_PENDING || s == DOWNLOAD_STATUS_DOWNLOADING)
            purge_ids[np++] = id;
        else
            keep_file_ids[nk++] = id;   /* COMPLETED / FAILED：仅移除记录 */
    }
    /* 先取消下载中/等待中（中止在传、删半成品文件），再移除已完成记录（保留音频）。
     * 两者都清持久化 JSON + 内存记录，按 id 匹配，互不影响。 */
    if (np > 0) podcast_controller_cancel_download_tasks(ctx->app, purge_ids, np);
    if (nk > 0) podcast_controller_delete_download_records(ctx->app, keep_file_ids, nk);
    if (np > 0 || nk > 0)
        page_navigator_navigate_to(&ctx->app->view->page_nav, ctx->app, PAGE_DOWNLOAD_TASK, NULL);
    free(keep_file_ids);
    free(purge_ids);
}

/* ---- Pause / Resume 按钮 ---- */
static void on_pause_resume_clicked(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    struct PodcastApp *app = ctx ? ctx->app : NULL;
    if (!app) return;
    if (podcast_controller_is_download_paused(app)) {
        podcast_controller_resume_all_downloads(app);
    } else {
        podcast_controller_pause_all_downloads(app);
    }
    /* Refresh the page to update button text */
    if (ctx && ctx->app) {
        page_navigator_navigate_to(&ctx->app->view->page_nav, ctx->app,
                                   PAGE_DOWNLOAD_TASK, NULL);
    }
}

/* ---- 删除按钮: 弹出二次确认 ---- */
static void on_delete_clicked(lv_event_t* e) {
    DownloadTaskPageCtx* ctx = lv_event_get_user_data(e);
    lv_obj_t* scr = lv_screen_active();
    ctx->confirm_sheet = lv_bottom_sheet_create(scr);
    lv_obj_add_event_cb(ctx->confirm_sheet->overlay, on_confirm_sheet_delete, LV_EVENT_DELETE, ctx);

    lv_obj_t* content = lv_bottom_sheet_get_content(ctx->confirm_sheet);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 16, 0);

    lv_obj_t* msg = lv_label_create(content);
    lv_label_set_text(msg, "Delete selected tasks?\n(Downloaded files are kept;\nunfinished ones are removed.)");
    lv_obj_set_style_text_color(msg, lv_color_hex(0x666666), 0);

    lv_obj_t* confirm = lv_button_create(content);
    lv_obj_set_size(confirm, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(confirm, 6, 0);
    lv_obj_add_event_cb(confirm, on_delete_confirm, LV_EVENT_CLICKED, ctx);
    lv_obj_t* cfl = lv_label_create(confirm);
    lv_label_set_text(cfl, "Delete");
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

/* ---- 构建统计卡片 ---- */
static lv_obj_t* build_stats_card(lv_obj_t* parent, struct PodcastApp* app) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    int pending, completed, failed;
    count_statuses(app, &pending, &completed, &failed);

    char est_buf[32];
    calc_est_time(app, est_buf, sizeof(est_buf));

    char pend_buf[8], comp_buf[8], fail_buf[8];
    snprintf(pend_buf, sizeof(pend_buf), "%d", pending);
    snprintf(comp_buf, sizeof(comp_buf), "%d", completed);
    snprintf(fail_buf, sizeof(fail_buf), "%d", failed);

    const char* vals[]   = {pend_buf, comp_buf, fail_buf, est_buf};
    const char* labels[] = {"Pending", "Done", "Failed", "ETA"};

    for (int i = 0; i < 4; i++) {
        lv_obj_t* item = lv_obj_create(card);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

        lv_obj_t* v = lv_label_create(item);
        lv_label_set_text(v, vals[i]);
        lv_obj_set_style_text_font(v, g_cjk_font, 0);
        /* Failed count uses a red accent when non-zero to draw the eye. */
        lv_color_t vcolor = (i == 2 && failed > 0)
                          ? lv_color_hex(0xD32F2F) : lv_color_hex(0x1976D2);
        lv_obj_set_style_text_color(v, vcolor, 0);

        lv_obj_t* l = lv_label_create(item);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, g_cjk_font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x888888), 0);
    }
    return card;
}

/* ---- 构建表头 (全选 switch | ALL | Title | Status) ---- */
static lv_obj_t* build_list_header(lv_obj_t* parent, DownloadTaskPageCtx* ctx) {
    lv_obj_t* hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, LV_PCT(100), 28);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0xFAFAFA), 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);

    /* 全选 switch */
    lv_obj_t* sw = lv_switch_create(hdr);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(sw, on_select_all_switch, LV_EVENT_VALUE_CHANGED, ctx);

    lv_obj_t* sw_label = lv_label_create(hdr);
    lv_label_set_text(sw_label, "ALL");
    lv_obj_align(sw_label, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_set_style_text_font(sw_label, g_cjk_font, 0);
    lv_obj_set_style_text_color(sw_label, lv_color_hex(0x666666), 0);

    /* Page nav: ◀ X/N ▶ */
    ctx->prev_btn = lv_button_create(hdr);
    lv_obj_set_size(ctx->prev_btn, 28, 22);
    lv_obj_align(ctx->prev_btn, LV_ALIGN_RIGHT_MID, -110, 0);
    lv_obj_set_style_pad_all(ctx->prev_btn, 0, 0);
    lv_obj_set_style_bg_opa(ctx->prev_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(ctx->prev_btn, 0, 0);
    lv_obj_set_style_border_width(ctx->prev_btn, 0, 0);
    lv_obj_t* pt = lv_label_create(ctx->prev_btn);
    lv_label_set_text(pt, "<"); lv_obj_center(pt);
    lv_obj_set_style_text_color(pt, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_text_font(pt, g_cjk_font, 0);

    ctx->page_label = lv_label_create(hdr);
    lv_label_set_text(ctx->page_label, "1/1");
    lv_obj_align(ctx->page_label, LV_ALIGN_RIGHT_MID, -78, 0);
    lv_obj_set_style_text_color(ctx->page_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(ctx->page_label, g_cjk_font, 0);

    ctx->next_btn = lv_button_create(hdr);
    lv_obj_set_size(ctx->next_btn, 28, 22);
    lv_obj_align(ctx->next_btn, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_set_style_pad_all(ctx->next_btn, 0, 0);
    lv_obj_set_style_bg_opa(ctx->next_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(ctx->next_btn, 0, 0);
    lv_obj_set_style_border_width(ctx->next_btn, 0, 0);
    lv_obj_t* nt = lv_label_create(ctx->next_btn);
    lv_label_set_text(nt, ">"); lv_obj_center(nt);
    lv_obj_set_style_text_color(nt, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_text_font(nt, g_cjk_font, 0);
    lv_obj_add_event_cb(ctx->prev_btn, dl_prev_page, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(ctx->next_btn, dl_next_page, LV_EVENT_CLICKED, ctx);

    /* Status 列头 */
    lv_obj_t* sh = lv_label_create(hdr);
    lv_label_set_text(sh, "Status");
    lv_obj_align(sh, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(sh, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(sh, g_cjk_font, 0);

    return hdr;
}

/* ---- 分页 ---- */

static void dl_prev_page(lv_event_t *e) {
    DownloadTaskPageCtx *ctx = lv_event_get_user_data(e);
    if (ctx && ctx->cur_page > 0) { ctx->cur_page--; build_task_list(ctx->list_container, ctx); }
}
static void dl_next_page(lv_event_t *e) {
    DownloadTaskPageCtx *ctx = lv_event_get_user_data(e);
    if (ctx && ctx->cur_page < ctx->total_pages - 1) { ctx->cur_page++; build_task_list(ctx->list_container, ctx); }
}

/* ---- 构建单个下载任务行 ---- */
static lv_obj_t* build_task_row(lv_obj_t* parent, const DownloadTask* task, int index,
                                 DownloadTaskPageCtx* ctx) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    /* checkbox */
    lv_obj_t* cb = lv_checkbox_create(row);
    lv_obj_set_width(cb, 24);
    lv_obj_align(cb, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_pad_all(cb, 0, 0);
    lv_obj_set_user_data(cb, (void*)(uintptr_t)index);
    lv_obj_add_event_cb(cb, on_checkbox_changed, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->task_cbs[index] = cb;

    /* 序号 */
    char num_buf[12];
    snprintf(num_buf, sizeof(num_buf), "%02d", index + 1);
    lv_obj_t* num = lv_label_create(row);
    lv_label_set_text(num, num_buf);
    lv_obj_align(num, LV_ALIGN_LEFT_MID, 32, 0);
    lv_obj_set_style_text_color(num, lv_color_hex(0x999999), 0);

    /* 标题 (曲目名) */
    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, task->episode_title);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 56, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);

    /* 状态区 (右对齐) */
    uint32_t status_color;
    const char* status_text;
    switch (task->status) {
        case DOWNLOAD_STATUS_PENDING:
            status_color = 0x888888; status_text = "Pending"; break;
        case DOWNLOAD_STATUS_DOWNLOADING:
            status_color = 0x1976D2; status_text = NULL; break;  /* 显示百分比 */
        case DOWNLOAD_STATUS_COMPLETED:
            status_color = 0x4CAF50; status_text = "Completed"; break;
        default:
            status_color = 0x888888; status_text = "?"; break;
    }

    lv_obj_t* st = lv_label_create(row);
    if (task->status == DOWNLOAD_STATUS_DOWNLOADING) {
        lv_label_set_text_fmt(st, "%d%%", task->progress);
    } else {
        lv_label_set_text(st, status_text);
    }
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(status_color), 0);

    /* 底部分隔线 */
    lv_obj_t* line = lv_obj_create(row);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    return row;
}

/* ---- 构建音频列表 ---- */
static lv_obj_t* build_task_list(lv_obj_t* parent, DownloadTaskPageCtx* ctx) {
    lv_obj_clean(parent);
    ctx->list_container = parent;

    const DownloadTask* tasks = podcast_model_get_download_tasks(ctx->app, &ctx->task_count);

    if (ctx->task_count == 0) {
        lv_obj_t* empty = lv_label_create(parent);
        lv_label_set_text(empty, "No download tasks");
        lv_obj_set_style_text_color(empty, lv_color_hex(0xAAAAAA), 0);
        lv_obj_center(empty);
        return parent;
    }

    ctx->total_pages = (ctx->task_count + DL_PER_PAGE - 1) / DL_PER_PAGE;
    if (ctx->total_pages < 1) ctx->total_pages = 1;
    if (ctx->cur_page >= ctx->total_pages) ctx->cur_page = ctx->total_pages - 1;
    if (ctx->cur_page < 0) ctx->cur_page = 0;

    int start = ctx->cur_page * DL_PER_PAGE;
    int end = start + DL_PER_PAGE;
    if (end > ctx->task_count) end = ctx->task_count;
    int visible = end - start;

    /* Free old checkbox arrays */
    free(ctx->task_checked); free(ctx->task_cbs);
    ctx->task_checked = (bool *)calloc(visible, sizeof(bool));
    ctx->task_cbs = (lv_obj_t **)calloc(visible, sizeof(lv_obj_t *));

    for (int i = 0; i < visible; i++)
        build_task_row(parent, &tasks[start + i], start + i, ctx);

    /* Update page label */
    if (ctx->page_label)
        lv_label_set_text_fmt(ctx->page_label, "%d/%d", ctx->cur_page + 1, ctx->total_pages);

    /* Enable/disable prev/next via text color (buttons are transparent) */
    if (ctx->prev_btn) {
        lv_obj_t *pl = lv_obj_get_child(ctx->prev_btn, 0);
        lv_obj_set_style_text_color(pl,
            ctx->cur_page > 0 ? lv_color_hex(0x1976D2) : lv_color_hex(0xCCCCCC), 0);
    }
    if (ctx->next_btn) {
        lv_obj_t *nl = lv_obj_get_child(ctx->next_btn, 0);
        lv_obj_set_style_text_color(nl,
            ctx->cur_page < ctx->total_pages - 1 ? lv_color_hex(0x1976D2) : lv_color_hex(0xCCCCCC), 0);
    }

    /* Reset selection */
    if (ctx->sel_label) lv_label_set_text(ctx->sel_label, "0");
    if (ctx->action_bar) lv_obj_add_flag(ctx->action_bar, LV_OBJ_FLAG_HIDDEN);

    return parent;
}

/* ---- 构建底部操作栏 (选中数 | spacer | Delete) ----
 * 与 view_channel.c 的 bar4 一致：挂在 page.screen 上、FLOATING 浮动图层、
 * 吸底对齐，浮在列表之上，避免被 flex 布局挤出屏幕外截断。 */
static lv_obj_t* build_action_bar(lv_obj_t* parent, DownloadTaskPageCtx* ctx) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_shadow_width(bar, 20, 0);
    lv_obj_set_style_shadow_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 选中计数 */
    ctx->sel_label = lv_label_create(bar);
    lv_obj_set_width(ctx->sel_label, 30);
    lv_label_set_text(ctx->sel_label, "0");
    lv_obj_set_style_text_align(ctx->sel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ctx->sel_label, lv_color_hex(0x1976D2), 0);

    /* 弹性空白 */
    lv_obj_t* sp = lv_obj_create(bar);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    /* Delete 按钮：已完成→删记录保留文件；下载中/等待中→取消并删半成品文件 */
    ctx->delete_btn = lv_button_create(bar);
    lv_obj_set_size(ctx->delete_btn, 80, 28);
    lv_obj_set_style_bg_color(ctx->delete_btn, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_radius(ctx->delete_btn, 6, 0);
    lv_obj_add_event_cb(ctx->delete_btn, on_delete_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_t* dl = lv_label_create(ctx->delete_btn);
    lv_label_set_text(dl, "Delete");
    lv_obj_center(dl);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFFFFFF), 0);

    /* Pause / Resume 按钮 — 在 Delete 右侧 */
    ctx->pause_btn = lv_button_create(bar);
    lv_obj_set_size(ctx->pause_btn, 80, 28);
    lv_obj_set_style_bg_color(ctx->pause_btn,
        podcast_controller_is_download_paused(ctx->app)
            ? lv_color_hex(0x4CAF50) : lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(ctx->pause_btn, 6, 0);
    lv_obj_add_event_cb(ctx->pause_btn, on_pause_resume_clicked, LV_EVENT_CLICKED, ctx);
    ctx->pause_label = lv_label_create(ctx->pause_btn);
    lv_label_set_text(ctx->pause_label,
        podcast_controller_is_download_paused(ctx->app) ? "Resume" : "Pause");
    lv_obj_center(ctx->pause_label);
    lv_obj_set_style_text_color(ctx->pause_label, lv_color_hex(0xFFFFFF), 0);

    ctx->action_bar = bar;
    return bar;
}

/* ---- 页面主构建函数 ---- */
static lv_obj_t* build_download_task_page(struct PodcastApp* app, void* user_data) {
    (void)user_data;
    Page page = lv_page_create("Download Task", true, page_navigator_navigate_back, &app->view->page_nav);

    static bool evt_registered = false;
    if (!evt_registered) {
        app_event_register(on_download_complete_event);
        evt_registered = true;
        printf("[dl_task] registered download-complete listener\n"); fflush(stdout);
    }

    /* Invalidate old page pointer BEFORE building the new one.
     * Using an LV_EVENT_DELETE callback races with lv_obj_delete_async:
     * the old screen's async delete can clear s_active_dl_page AFTER the
     * new page has already set it.  Clearing it here is synchronous and safe. */
    s_active_dl_page = NULL;

    lv_obj_set_style_bg_color(page.container, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_pad_all(page.container, 8, 0);
    lv_obj_set_style_pad_row(page.container, 6, 0);

    /* 上下文 */
    DownloadTaskPageCtx* ctx = malloc(sizeof(DownloadTaskPageCtx));
    memset(ctx, 0, sizeof(DownloadTaskPageCtx));
    ctx->app = app;
    int count = app->model->download_task_count;
    if (count > 0) {
        ctx->task_checked = malloc(sizeof(bool) * count);
        memset(ctx->task_checked, 0, sizeof(bool) * count);
        ctx->task_cbs = malloc(sizeof(lv_obj_t*) * count);
        memset(ctx->task_cbs, 0, sizeof(lv_obj_t*) * count);
    }

    /* 统计卡片 */
    build_stats_card(page.container, app);

    /* 表头 (全选 switch | ALL | Title | Status) */
    build_list_header(page.container, ctx);

    /* 音频列表 — flex_grow=1 fills remaining space between header and action bar */
    lv_obj_t* list_container = lv_obj_create(page.container);
    lv_obj_set_size(list_container, LV_PCT(100), 0);  /* height=0, flex_grow expands */
    lv_obj_set_flex_grow(list_container, 1);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_bg_color(list_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_AUTO);
    build_task_list(list_container, ctx);

    /* 操作栏 — 浮动吸底图层，挂在 page.screen 上（而非 flex 容器），
     * 浮在列表之上，避免按钮被 flex 布局挤出屏幕截断。
     * (Must be created after the list so ctx->action_bar is valid.) */
    build_action_bar(page.screen, ctx);

    s_active_dl_page = ctx;

    printf("[INF] Download Task page built (%d tasks)\n", count);
    fflush(stdout);
    return page.screen;
}

void podcast_view_download_task_init_registry(struct PodcastApp* app) {
    PAGE_REGISTE(app, PAGE_DOWNLOAD_TASK, build_download_task_page);
}
