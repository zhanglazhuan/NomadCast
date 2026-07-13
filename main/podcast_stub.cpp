static lv_obj_t *g_podcast_screen = NULL;

static void podcast_start(lv_obj_t *root, lv_group_t *group) {
    (void)root; (void)group;
    g_podcast_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_podcast_screen, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_bg_opa(g_podcast_screen, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(g_podcast_screen);
    lv_label_set_text(title, "Podcast");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *sub = lv_label_create(g_podcast_screen);
    lv_label_set_text(sub, "Coming Soon");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8899AA), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -20);
    lv_obj_t *btn = lv_button_create(g_podcast_screen);
    lv_obj_set_size(btn, 100, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_add_event_cb(btn, page_navigator_navigate_back, LV_EVENT_CLICKED, NULL);
    lv_scr_load(g_podcast_screen);
}
static void podcast_stop(void) {
    if (g_podcast_screen) { lv_obj_del(g_podcast_screen); g_podcast_screen = NULL; }
}
