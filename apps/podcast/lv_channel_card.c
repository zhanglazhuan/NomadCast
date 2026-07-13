/**
 * @file lv_channel_card.c
 * @brief Shared channel card widget — cover image + icon rows + progress bar
 *
 * Layout (88px tall flex row):
 *   [cover 60x60] [info column]
 *                    [title (CJK, ellipsis)]
 *                    [clock icon 12x12 + upload_time]
 *                    [stacks icon 12x12 + "%d episodes"]
 *                    [progress bar, 4px]
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lv_channel_card.h"
#include "cache.h"

extern const lv_image_dsc_t ic_schedule_16x16;
extern const lv_image_dsc_t ic_stacks_16x16;
/* g_cjk_font declared in model.h */

/* Helper: build a tiny flex row with icon + label */
static lv_obj_t *icon_row(lv_obj_t *parent, const lv_image_dsc_t *icon) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_image_create(row);
    lv_image_set_src(img, icon);
    /* gray color baked into pixel data */

    return row;
}

lv_obj_t *lv_channel_card_create(lv_obj_t *parent, const Channel *a) {
    /* ── Card root ─────────────────────────────────────────────────────── */
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, LV_PCT(90), CARD_HEIGHT);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 10, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_user_data(card, (void *)(uintptr_t)a->id);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* ── Cover image ───────────────────────────────────────────────────── */
    lv_obj_t *cover = lv_image_create(card);
    lv_obj_set_size(cover, CARD_COVER_SZ, CARD_COVER_SZ);
    lv_obj_set_style_radius(cover, 6, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(a->card_color), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_CLICKABLE);

    /* Cached artwork from SD card */
    char art_lvgl[256], art_real[256];
    snprintf(art_lvgl, sizeof(art_lvgl),
             "S:.podcast/cache/artwork/%d.png", a->collection_id);
    snprintf(art_real, sizeof(art_real),
             "/sdcard/.podcast/cache/artwork/%d.png", a->collection_id);
    FILE *check = fopen(art_real, "rb");
    if (check) { fclose(check); lv_image_set_src(cover, art_lvgl); }

    /* ── Info column ───────────────────────────────────────────────────── */
    lv_obj_t *info = lv_obj_create(card);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_row(info, 2, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_CLICKABLE);

    /* Row 1: Title */
    lv_obj_t *tl = lv_label_create(info);
    lv_label_set_text(tl, a->title);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(tl, g_cjk_font, 0);
    lv_obj_set_width(tl, LV_PCT(100));

    /* Row 2: clock icon + upload_time */
    lv_obj_t *ut_row = icon_row(info, &ic_schedule_16x16);
    lv_obj_t *ut = lv_label_create(ut_row);
    lv_label_set_text(ut, a->upload_time[0] ? a->upload_time : "--");
    lv_obj_set_style_text_color(ut, lv_color_hex(0x999999), 0);

    /* Row 3: stacks icon + episode count */
    lv_obj_t *ep_row = icon_row(info, &ic_stacks_16x16);
    lv_obj_t *ep = lv_label_create(ep_row);
    lv_label_set_text_fmt(ep, "%d episodes", a->episode_count);
    lv_obj_set_style_text_color(ep, lv_color_hex(0x999999), 0);

    /* Row 4: Progress bar */
    lv_obj_t *pbar = lv_bar_create(info);
    lv_obj_set_size(pbar, LV_PCT(100), 4);
    lv_obj_set_style_pad_top(pbar, 2, 0);
    lv_obj_set_style_bg_color(pbar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_radius(pbar, 2, 0);
    lv_bar_set_range(pbar, 0, 100);

    int pct = cache_playback_channel_progress_pct(a->id, a->episode_count);
    lv_bar_set_value(pbar, pct, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(pbar, lv_color_hex(0x1976D2),
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(pbar, 2, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    return card;
}
