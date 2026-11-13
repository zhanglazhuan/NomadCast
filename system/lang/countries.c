/**
 * @file countries.c
 * @brief Apple Podcasts storefront country table + per-country timezone table.
 */
#include "countries.h"
#include "lang.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *tz;   /* POSIX TZ string — offset sign is inverted vs. UTC */
    const char *zh;   /* localized label (zh-CN) */
    const char *en;   /* localized label (en) */
} timezone_t;

typedef struct {
    const char *code;
    const char *zh;
    const char *en;
    const timezone_t *zones;
    int zone_count;
} country_t;

/* ── Per-country timezone tables ────────────────────────────────────────────
 * Each country lists its standard-time zones. Zone 0 is the default that is
 * applied when the country is selected; multi-zone countries expose the rest
 * as dropdown choices. Offsets are fixed (no DST), matching clock.c's POSIX
 * TZ convention: sign is inverted ("CST-8" = UTC+8, "EST+5" = UTC-5). */

static const timezone_t tz_cn[] = { { "CST-8", "UTC+8 北京", "UTC+8 Beijing" } };
static const timezone_t tz_us[] = {
    { "EST+5",  "UTC-5 东部",   "UTC-5 Eastern" },
    { "CST+6",  "UTC-6 中部",   "UTC-6 Central" },
    { "MST+7",  "UTC-7 山地",   "UTC-7 Mountain" },
    { "PST+8",  "UTC-8 太平洋", "UTC-8 Pacific" },
    { "AKST+9", "UTC-9 阿拉斯加", "UTC-9 Alaska" },
    { "HST+10", "UTC-10 夏威夷", "UTC-10 Hawaii" },
};
static const timezone_t tz_gb[] = { { "GMT0", "UTC+0 伦敦", "UTC+0 London" } };
static const timezone_t tz_ca[] = {
    { "AST+4",     "UTC-4 大西洋",  "UTC-4 Atlantic" },
    { "EST+5",     "UTC-5 东部",    "UTC-5 Eastern" },
    { "CST+6",     "UTC-6 中部",    "UTC-6 Central" },
    { "MST+7",     "UTC-7 山地",    "UTC-7 Mountain" },
    { "PST+8",     "UTC-8 太平洋",  "UTC-8 Pacific" },
    { "NST+3:30",  "UTC-3:30 纽芬兰", "UTC-3:30 Newfoundland" },
};
static const timezone_t tz_au[] = {
    { "AEST-10",   "UTC+10 东部", "UTC+10 Eastern" },
    { "ACST-9:30", "UTC+9:30 中部", "UTC+9:30 Central" },
    { "AWST-8",    "UTC+8 西部",  "UTC+8 Western" },
};
static const timezone_t tz_jp[] = { { "JST-9", "UTC+9 东京", "UTC+9 Tokyo" } };
static const timezone_t tz_kr[] = { { "KST-9", "UTC+9 首尔", "UTC+9 Seoul" } };
static const timezone_t tz_hk[] = { { "HKT-8", "UTC+8 香港", "UTC+8 Hong Kong" } };
static const timezone_t tz_tw[] = { { "CST-8", "UTC+8 台北", "UTC+8 Taipei" } };
static const timezone_t tz_sg[] = { { "SGT-8", "UTC+8 新加坡", "UTC+8 Singapore" } };
static const timezone_t tz_de[] = { { "CET-1", "UTC+1 柏林", "UTC+1 Berlin" } };
static const timezone_t tz_fr[] = { { "CET-1", "UTC+1 巴黎", "UTC+1 Paris" } };
static const timezone_t tz_it[] = { { "CET-1", "UTC+1 罗马", "UTC+1 Rome" } };
static const timezone_t tz_es[] = { { "CET-1", "UTC+1 马德里", "UTC+1 Madrid" } };
static const timezone_t tz_nl[] = { { "CET-1", "UTC+1 阿姆斯特丹", "UTC+1 Amsterdam" } };
static const timezone_t tz_se[] = { { "CET-1", "UTC+1 斯德哥尔摩", "UTC+1 Stockholm" } };
static const timezone_t tz_no[] = { { "CET-1", "UTC+1 奥斯陆", "UTC+1 Oslo" } };
static const timezone_t tz_dk[] = { { "CET-1", "UTC+1 哥本哈根", "UTC+1 Copenhagen" } };
static const timezone_t tz_fi[] = { { "EET-2", "UTC+2 赫尔辛基", "UTC+2 Helsinki" } };
static const timezone_t tz_br[] = {
    { "BRT+3", "UTC-3 巴西利亚", "UTC-3 Brasília" },
    { "AMT+4", "UTC-4 马瑙斯",   "UTC-4 Manaus" },
};
static const timezone_t tz_mx[] = {
    { "CST+6", "UTC-6 墨西哥城", "UTC-6 Mexico City" },
    { "MST+7", "UTC-7 奇瓦瓦",   "UTC-7 Chihuahua" },
    { "PST+8", "UTC-8 蒂华纳",   "UTC-8 Tijuana" },
};
static const timezone_t tz_in[] = { { "IST-5:30", "UTC+5:30 新德里", "UTC+5:30 New Delhi" } };
static const timezone_t tz_id[] = {
    { "WIB-7",  "UTC+7 雅加达",    "UTC+7 Jakarta" },
    { "WITA-8", "UTC+8 巴厘岛",    "UTC+8 Bali" },
    { "WIT-9",  "UTC+9 查亚普拉",  "UTC+9 Jayapura" },
};
static const timezone_t tz_my[] = { { "MYT-8", "UTC+8 吉隆坡", "UTC+8 Kuala Lumpur" } };
static const timezone_t tz_th[] = { { "ICT-7", "UTC+7 曼谷", "UTC+7 Bangkok" } };
static const timezone_t tz_vn[] = { { "ICT-7", "UTC+7 河内", "UTC+7 Hanoi" } };

/* Curated major markets. The storefront code is what the server passes to Apple.
 * Add/remove entries here to change the dropdown — no other code needs to change. */
static const country_t g_countries[] = {
    {"cn", "中国大陆", "China",          tz_cn, 1},
    {"us", "美国",     "United States",  tz_us, 6},
    {"gb", "英国",     "United Kingdom", tz_gb, 1},
    {"ca", "加拿大",   "Canada",         tz_ca, 6},
    {"au", "澳大利亚", "Australia",      tz_au, 3},
    {"jp", "日本",     "Japan",          tz_jp, 1},
    {"kr", "韩国",     "South Korea",    tz_kr, 1},
    {"hk", "香港",     "Hong Kong",      tz_hk, 1},
    {"tw", "台湾",     "Taiwan",         tz_tw, 1},
    {"sg", "新加坡",   "Singapore",      tz_sg, 1},
    {"de", "德国",     "Germany",        tz_de, 1},
    {"fr", "法国",     "France",         tz_fr, 1},
    {"it", "意大利",   "Italy",          tz_it, 1},
    {"es", "西班牙",   "Spain",          tz_es, 1},
    {"nl", "荷兰",     "Netherlands",    tz_nl, 1},
    {"se", "瑞典",     "Sweden",         tz_se, 1},
    {"no", "挪威",     "Norway",         tz_no, 1},
    {"dk", "丹麦",     "Denmark",        tz_dk, 1},
    {"fi", "芬兰",     "Finland",        tz_fi, 1},
    {"br", "巴西",     "Brazil",         tz_br, 2},
    {"mx", "墨西哥",   "Mexico",         tz_mx, 3},
    {"in", "印度",     "India",          tz_in, 1},
    {"id", "印度尼西亚", "Indonesia",    tz_id, 3},
    {"my", "马来西亚", "Malaysia",       tz_my, 1},
    {"th", "泰国",     "Thailand",       tz_th, 1},
    {"vn", "越南",     "Vietnam",        tz_vn, 1},
};

#define COUNTRY_COUNT ((int)(sizeof(g_countries) / sizeof(g_countries[0])))

int country_count(void)
{
    return COUNTRY_COUNT;
}

const char *country_code_at(int idx)
{
    if (idx < 0 || idx >= COUNTRY_COUNT) return "cn";
    return g_countries[idx].code;
}

const char *country_label_at(int idx)
{
    if (idx < 0 || idx >= COUNTRY_COUNT) idx = 0;
    return (lang_get() == LANG_ZH_CN) ? g_countries[idx].zh : g_countries[idx].en;
}

int country_index_of(const char *code)
{
    if (!code) return -1;
    for (int i = 0; i < COUNTRY_COUNT; i++) {
        if (strcmp(g_countries[i].code, code) == 0) return i;
    }
    return -1;
}

int country_options_str(char *buf, int sz)
{
    int off = 0;
    for (int i = 0; i < COUNTRY_COUNT; i++) {
        const char *label = country_label_at(i);
        int n = snprintf(buf + off, (sz > off) ? (sz - off) : 0, "%s%s",
                         (i > 0) ? "\n" : "", label);
        if (n < 0) break;
        off += n;
        if (off >= sz - 1) break;
    }
    return COUNTRY_COUNT;
}

/* ── Timezones ─────────────────────────────────────────────────────────────── */

int country_timezone_count(int idx)
{
    if (idx < 0 || idx >= COUNTRY_COUNT) return 0;
    return g_countries[idx].zone_count;
}

const char *country_timezone_tz_at(int idx, int zone)
{
    if (idx < 0 || idx >= COUNTRY_COUNT) return "CST-8";
    const country_t *c = &g_countries[idx];
    if (zone < 0 || zone >= c->zone_count) zone = 0;
    return c->zones[zone].tz;
}

const char *country_timezone_label_at(int idx, int zone)
{
    if (idx < 0 || idx >= COUNTRY_COUNT) idx = 0;
    const country_t *c = &g_countries[idx];
    if (zone < 0 || zone >= c->zone_count) zone = 0;
    return (lang_get() == LANG_ZH_CN) ? c->zones[zone].zh : c->zones[zone].en;
}

int country_timezone_options_str(int idx, char *buf, int sz)
{
    int off = 0;
    int n = country_timezone_count(idx);
    for (int i = 0; i < n; i++) {
        const char *label = country_timezone_label_at(idx, i);
        int w = snprintf(buf + off, (sz > off) ? (sz - off) : 0, "%s%s",
                         (i > 0) ? "\n" : "", label);
        if (w < 0) break;
        off += w;
        if (off >= sz - 1) break;
    }
    return n;
}

int country_timezone_index_of_tz(int idx, const char *tz)
{
    if (idx < 0 || idx >= COUNTRY_COUNT || !tz) return -1;
    const country_t *c = &g_countries[idx];
    for (int i = 0; i < c->zone_count; i++) {
        if (strcmp(c->zones[i].tz, tz) == 0) return i;
    }
    return -1;
}
