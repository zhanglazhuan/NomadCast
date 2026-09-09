/*
 * NomadCast — global UI language state + translation lookup
 *
 * Single source of truth for the current UI language. Strings live in
 * `strings_zh_cn.c` / `strings_en.c` (one `const char *const[]` per language,
 * index-aligned with the STR_* enum in `lang_strings.h`).
 */

#pragma once

#include "lang_strings.h" /* STR_* enum keys for tr() */

/* Build-time default language. Defined by sdkconfig.h in IDF builds (via
 * CONFIG_NOMADCAST_DEFAULT_LANGUAGE); fall back to Simplified Chinese for
 * pc_demo and any non-IDF build that lacks sdkconfig.h. */
#ifndef CONFIG_NOMADCAST_DEFAULT_LANGUAGE
#define CONFIG_NOMADCAST_DEFAULT_LANGUAGE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_ZH_CN = 0,
    LANG_EN = 1,
    LANG_COUNT
} lang_t;

/* Load persisted language from NVS (settings/lang) applying the build default. */
void lang_init(void);

/* Current language. */
lang_t lang_get(void);

/* Switch the global language. Persistence is the caller's job (flash_set_i32). */
void lang_set(lang_t lang);

/* STR_* enum -> localized string for the current language. */
const char *tr(int key);

/* Stable app key ("Podcast"/"Settings") -> localized display name. */
const char *app_name_tr(const char *app_key);

#ifdef __cplusplus
}
#endif
