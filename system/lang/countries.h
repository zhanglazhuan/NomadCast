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

#ifdef __cplusplus
}
#endif

#endif /* COUNTRIES_H */
