/*
 * pinyin_zh_cn.h — Simplified-first pinyin dictionary for lv_ime_pinyin.
 *
 * LVGL's built-in default dictionary (lv_ime_pinyin_def_dict) is Traditional /
 * Japanese biased, so zh-CN users see 繁体 first (and odd Japanese variants).
 * This shared dictionary keeps the same 321 pinyin keys in the same order as
 * the default (so lv_ime_pinyin's first-letter index stays valid), but reorders
 * every candidate string to Simplified-first / Traditional-second and drops the
 * Japanese-only glyphs (氷 変 辺 咲 込 姉 戻 歩 駄 …).
 */

#pragma once

#include "lvgl.h"   /* lv_pinyin_dict_t */

extern const lv_pinyin_dict_t g_pinyin_zh_cn_dict[];
