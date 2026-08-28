/**
 * @file lv_channel_card.c
 * @brief Channel card — lightweight: 6 LVGL objects per card.
 *
 * Layout (72px tall): [cover 60x60] [title | upload_time | # episodes]
 */
#include <stdio.h>
#include "lv_channel_card.h"

lv_obj_t *lv_channel_card_create(lv_obj_t *parent, const Channel *a) {
    /* ── Card root ─────────────────────────────────────────────────────── */
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, LV_PCT(98), CARD_HEIGHT);
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

    /* Artwork: controlled by LV_CHANNEL_CARD_SHOW_ARTWORK in the header.
     * Off → random solid color (a->card_color, already set above).
     * On  → LVGL loads the cached PNG from SD asynchronously. */
#if LV_CHANNEL_CARD_SHOW_ARTWORK
    char art_lvgl[256];
    snprintf(art_lvgl, sizeof(art_lvgl),
             "S:.nomadcast/cache/artwork/%d.png", a->collection_id);
    lv_image_set_src(cover, art_lvgl);
#endif

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

    /* Row 2: upload_time (no icon — saves 3 objects per card) */
    lv_obj_t *ut = lv_label_create(info);
    lv_label_set_text(ut, a->upload_time[0] ? a->upload_time : "--");
    lv_obj_set_style_text_color(ut, lv_color_hex(0x999999), 0);

    /* Row 3: episode count */
    lv_obj_t *ep = lv_label_create(info);
    lv_label_set_text_fmt(ep, "%d episodes", a->episode_count);
    lv_obj_set_style_text_color(ep, lv_color_hex(0x999999), 0);

    return card;
}
