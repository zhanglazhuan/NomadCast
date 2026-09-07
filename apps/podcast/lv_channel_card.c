/**
 * @file lv_channel_card.c
 * @brief Channel card — two-row layout.
 *
 *   ┌──────────────────────────────────────────┐
 *   │ [cover 60x60]  title title title (≤3行) │  ← top: cover + title
 *   │                title title...            │     (wrap; ellipsis if >3 lines)
 *   ├──────────────────────────────────────────┤
 *   │ 12 episodes                 2024-03-15   │  ← bottom: count (left) + time (right)
 *   └──────────────────────────────────────────┘
 */
#include <stdio.h>
#include "lv_channel_card.h"

lv_obj_t *lv_channel_card_create(lv_obj_t *parent, const Channel *a) {
    int32_t line_h = lv_font_get_line_height(g_cjk_font);
    /* card height = top(cover) + gap + bottom(one line) + vertical padding */
    int32_t card_h = CARD_COVER_SZ + CARD_GAP + line_h + 2 * CARD_PAD;

    /* ── Card root ─────────────────────────────────────────────────────── */
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, LV_PCT(98), card_h);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_user_data(card, (void *)(uintptr_t)a->id);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* ── Top row: cover (left) + title (right) ─────────────────────────── */
    lv_obj_t *top = lv_obj_create(card);
    lv_obj_set_size(top, LV_PCT(100), CARD_COVER_SZ);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_column(top, 10, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(top, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);

    /* Cover image */
    lv_obj_t *cover = lv_image_create(top);
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

    /* Title — up to 3 lines; wraps, then ellipsis ("...") if still too long */
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, a->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_height(title, line_h * 3);
    lv_obj_set_style_text_font(title, g_cjk_font, 0);

    /* ── Bottom row: episode count (left) + upload time (right) ────────── */
    lv_obj_t *bottom = lv_obj_create(card);
    lv_obj_set_size(bottom, LV_PCT(100), line_h);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(bottom, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ep = lv_label_create(bottom);
    lv_label_set_text_fmt(ep, "%d episodes", a->episode_count);
    lv_obj_set_style_text_color(ep, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(ep, g_cjk_font, 0);
    lv_obj_align(ep, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *ut = lv_label_create(bottom);
    lv_label_set_text(ut, a->upload_time[0] ? a->upload_time : "--");
    lv_obj_set_style_text_color(ut, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(ut, g_cjk_font, 0);
    lv_obj_align(ut, LV_ALIGN_RIGHT_MID, 0, 0);

    return card;
}
