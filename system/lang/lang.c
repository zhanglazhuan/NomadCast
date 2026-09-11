/*
 * NomadCast — global UI language state + translation lookup
 *
 * The language is stored in NVS under namespace "settings", key "lang"
 * (0 = Simplified Chinese, 1 = English). `CONFIG_NOMADCAST_DEFAULT_LANGUAGE`
 * supplies the build-time default (0 for the domestic build, 1 for the
 * overseas build). At runtime the user can override it in
 * Settings → General → Language.
 */

#include "lang.h"
#include "lang_strings.h"
#include "flash_store.h"
#include <string.h>

extern const char *const strings_zh_cn[];
extern const char *const strings_en[];

static lang_t s_lang = LANG_ZH_CN;
static const char *const *s_table = strings_zh_cn;

void lang_init(void) {
    int idx = flash_get_i32("settings", "lang", CONFIG_NOMADCAST_DEFAULT_LANGUAGE);
    lang_set((lang_t)idx);
}

lang_t lang_get(void) {
    return s_lang;
}

void lang_set(lang_t lang) {
    if (lang < 0 || lang >= LANG_COUNT) {
        lang = LANG_ZH_CN;
    }
    s_lang = lang;
    s_table = (lang == LANG_ZH_CN) ? strings_zh_cn : strings_en;
}

const char *tr(int key) {
    if (key < 0 || key >= STR_COUNT) {
        return "";
    }
    return s_table[key];
}

const char *app_name_tr(const char *app_key) {
    if (!app_key) {
        return "";
    }
    if (strcmp(app_key, "Podcast") == 0) {
        return tr(STR_APP_PODCAST);
    }
    if (strcmp(app_key, "Player") == 0) {
        return tr(STR_APP_PLAYER);
    }
    if (strcmp(app_key, "Radio") == 0) {
        return tr(STR_APP_RADIO);
    }
    if (strcmp(app_key, "Settings") == 0) {
        return tr(STR_APP_SETTINGS);
    }
    return app_key; /* unknown key — return as-is */
}
