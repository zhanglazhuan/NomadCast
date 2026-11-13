/**
 * @file countries.h
 * @brief Apple Podcasts storefront country table (code ↔ localized name)
 *
 * `country` in /api/charts/full is an Apple storefront 2-letter code. This table
 * maps a curated set of major markets to bilingual (zh/en) display names, shared
 * by the Settings → General dropdown and the Podcast settings dropdown.
 */
#ifndef COUNTRIES_H
#define COUNTRIES_H

#ifdef __cplusplus
extern "C" {
#endif

/** Number of countries in the table. */
int  country_count(void);

/** 2-letter lowercase storefront code at index (e.g. "cn"). Returns "cn" if idx is out of range. */
const char *country_code_at(int idx);

/** Localized display name at index, chosen by the current language (lang_get()). */
const char *country_label_at(int idx);

/** Index of a storefront code, or -1 if not present. */
int  country_index_of(const char *code);

/** Fill buf (size sz) with a '\n'-joined list of localized names; returns the entry count. */
int  country_options_str(char *buf, int sz);

/* ── Timezones (per-country) ───────────────────────────────────────────────
 * Each country maps to one or more standard-time zones. Zone 0 is the default
 * applied when the country is selected; the others are extra dropdown choices.
 * A country's POSIX TZ string ("CST-8") is what clock_set_timezone() consumes. */

/** Number of timezones for the country at idx (0 if idx out of range). */
int  country_timezone_count(int idx);

/** POSIX TZ string for the country's zone `zone` (0-based); falls back to the
 *  default zone when `zone` is out of range, "CST-8" when idx is invalid. */
const char *country_timezone_tz_at(int idx, int zone);

/** Localized display label for the country's zone `zone`. */
const char *country_timezone_label_at(int idx, int zone);

/** Fill buf with a '\n'-joined list of localized timezone labels for country idx;
 *  returns the zone count. */
int  country_timezone_options_str(int idx, char *buf, int sz);

/** Zone index (0-based) whose POSIX TZ string matches `tz`, or -1 if not found. */
int  country_timezone_index_of_tz(int idx, const char *tz);

#ifdef __cplusplus
}
#endif

#endif /* COUNTRIES_H */
