/**
 * @file countries.c
 * @brief Apple Podcasts storefront country table — implementation.
 */
#include "countries.h"
#include "lang.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *code;
    const char *zh;
    const char *en;
} country_t;

/* Curated major markets. The storefront code is what the server passes to Apple.
 * Add/remove entries here to change the dropdown — no other code needs to change. */
static const country_t g_countries[] = {
    {"cn", "中国大陆", "China"},
    {"us", "美国",     "United States"},
    {"gb", "英国",     "United Kingdom"},
    {"ca", "加拿大",   "Canada"},
    {"au", "澳大利亚", "Australia"},
    {"jp", "日本",     "Japan"},
    {"kr", "韩国",     "South Korea"},
    {"hk", "香港",     "Hong Kong"},
    {"tw", "台湾",     "Taiwan"},
    {"sg", "新加坡",   "Singapore"},
    {"de", "德国",     "Germany"},
    {"fr", "法国",     "France"},
    {"it", "意大利",   "Italy"},
    {"es", "西班牙",   "Spain"},
    {"nl", "荷兰",     "Netherlands"},
    {"se", "瑞典",     "Sweden"},
    {"no", "挪威",     "Norway"},
    {"dk", "丹麦",     "Denmark"},
    {"fi", "芬兰",     "Finland"},
    {"br", "巴西",     "Brazil"},
    {"mx", "墨西哥",   "Mexico"},
    {"in", "印度",     "India"},
    {"id", "印度尼西亚", "Indonesia"},
    {"my", "马来西亚", "Malaysia"},
    {"th", "泰国",     "Thailand"},
    {"vn", "越南",     "Vietnam"},
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
