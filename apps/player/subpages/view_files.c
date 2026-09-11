#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "view_files.h"
#include "view_tab_bar.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lang.h"

extern PlayerApp g_player_app;
extern const lv_font_t *g_cjk_font;

typedef struct {
    char  name[PLAYER_PATH_MAX];
    bool  is_dir;
} FileEntry;

/* Case-insensitive string compare (avoids strcasecmp portability concerns). */
static int str_icmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Audio extensions the decoder can open (case-insensitive). */
static bool is_audio_file(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    static const char *aud[] = { "mp3", "aac", "m4a", "m4b", "ts" };
    for (int i = 0; i < 5; i++) {
        if (str_icmp(ext, aud[i]) == 0) return true;
    }
    return false;
}

/* Directories first, then case-insensitive name order. */
static int entry_cmp(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return str_icmp(ea->name, eb->name);
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Centered empty-state hint (mirrors podcast build_hint). FLOATING flag lets
 * it escape the container's flex-column layout so it truly centers. */
static void show_empty(lv_obj_t *parent, const char *msg) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label, g_cjk_font, 0);
    lv_obj_set_size(label, 200, 60);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

/* ── row user_data (full path) cleanup ─────────────────────────────────── */

static void free_row_user(lv_event_t *e) {
    char *p = (char *)lv_event_get_user_data(e);
    if (p) free(p);
}

/* ── row click handlers ────────────────────────────────────────────────── */

static void on_dir_clicked(lv_event_t *e) {
    const char *full = (const char *)lv_event_get_user_data(e);
    if (!full) return;
    strncpy(g_player_app.model->current_dir, full, PLAYER_PATH_MAX - 1);
    g_player_app.model->current_dir[PLAYER_PATH_MAX - 1] = '\0';
    player_nav_replace(&g_player_app, PAGE_FILES);
}

static void on_file_clicked(lv_event_t *e) {
    const char *full = (const char *)lv_event_get_user_data(e);
    if (!full) return;
    player_controller_play_file(&g_player_app, full);
    player_nav_push(&g_player_app, PAGE_FILES, PAGE_PLAYER);
}

/* ── "up one level" (header back button in a subfolder) ────────────────── */

static void on_dir_up(lv_event_t *e) {
    struct PlayerApp *app = (struct PlayerApp *)lv_event_get_user_data(e);
    player_go_up(app);
}

/* ── page builder ──────────────────────────────────────────────────────── */

static lv_obj_t *build_files_page(struct PlayerApp *app, void *user_data) {
    (void)user_data;  /* directory is tracked in model->current_dir */

    const char *path = app->model->current_dir[0] ? app->model->current_dir : PLAYER_SD_ROOT;
    bool is_root = (strcmp(path, PLAYER_SD_ROOT) == 0);

    Page page = lv_page_create(is_root ? tr(STR_TAB_FILES) : basename_of(path),
                               !is_root, on_dir_up, app);
    lv_obj_t *cont = page.container;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    DIR *d = opendir(path);
    if (!d) {
        show_empty(cont, tr(STR_NO_SD_CARD));
        if (is_root) player_view_create_bottom_tab_bar(page.screen, PLAYER_TAB_FILES);
        return page.screen;
    }

    int cap = 16, n = 0;
    FileEntry *entries = (FileEntry *)malloc(sizeof(FileEntry) * cap);
    if (!entries) { closedir(d); return page.screen; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char full[PLAYER_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !S_ISREG(st.st_mode)) continue;
        if (!is_dir && !is_audio_file(de->d_name)) continue;

        if (n == cap) {
            cap *= 2;
            FileEntry *tmp = (FileEntry *)realloc(entries, sizeof(FileEntry) * cap);
            if (!tmp) break;
            entries = tmp;
        }
        strncpy(entries[n].name, de->d_name, sizeof(entries[n].name) - 1);
        entries[n].name[sizeof(entries[n].name) - 1] = '\0';
        entries[n].is_dir = is_dir;
        n++;
    }
    closedir(d);

    qsort(entries, n, sizeof(FileEntry), entry_cmp);

    if (n == 0) {
        free(entries);
        show_empty(cont, is_root ? tr(STR_PLAYER_NO_AUDIO) : tr(STR_PLAYER_EMPTY_DIR));
        if (is_root) player_view_create_bottom_tab_bar(page.screen, PLAYER_TAB_FILES);
        return page.screen;
    }

    for (int i = 0; i < n; i++) {
        char *full = (char *)malloc(PLAYER_PATH_MAX * 2);
        snprintf(full, PLAYER_PATH_MAX * 2, "%s/%s", path, entries[i].name);

        lv_obj_t *row = lv_button_create(cont);
        lv_obj_set_size(row, LV_PCT(100), 40);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(ic, g_cjk_font, 0);
        lv_obj_set_style_text_color(ic, entries[i].is_dir
                                        ? lv_color_hex(0x1976D2)
                                        : lv_color_hex(0x666666), 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, entries[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name, g_cjk_font, 0);
        lv_obj_set_width(name, 185);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 44, 0);

        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, LV_PCT(100), 1);
        lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

        if (entries[i].is_dir) {
            lv_obj_add_event_cb(row, on_dir_clicked, LV_EVENT_CLICKED, full);
        } else {
            lv_obj_add_event_cb(row, on_file_clicked, LV_EVENT_CLICKED, full);
        }
        lv_obj_add_event_cb(row, free_row_user, LV_EVENT_DELETE, full);
    }

    free(entries);

    if (is_root) player_view_create_bottom_tab_bar(page.screen, PLAYER_TAB_FILES);

    return page.screen;
}

void player_view_files_init_registry(struct PlayerApp *app) {
    PAGE_REGISTE(app, PAGE_FILES, build_files_page);
}
