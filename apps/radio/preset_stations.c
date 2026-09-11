#include "preset_stations.h"

/* Built-in internet radio presets, split by UI language.
 *
 * These are DIRECT MP3/AAC HTTP(S) streams (Icecast/Shoutcast). The ADF
 * http_stream → decoder pipeline only plays simple progressive streams —
 * HLS (.m3u8) requires a separate hls_stream element and is NOT supported by
 * this app.
 *
 * Chinese (LANG_ZH_CN): Qingting FM (蜻蜓FM) direct 64 kbps MP3 mirrors of the
 * major national (CNR) + provincial stations. The official CNR CDN
 * (ngcdn.cnr.cn / satellitepull.cnr.cn) now serves HLS only, so we use these
 * third-party MP3 mirrors instead. Swap any URL — hotlink policies change over
 * time (https://lhttp.qingting.fm/... works too if a redirect is needed).
 *
 * English (LANG_EN): SomaFM (San Francisco) — stable, direct 128 kbps MP3. */

/* ── Chinese: 央广 + 主要省市台（蜻蜓FM 64k.mp3 直链）────────────────── */
const radio_station_t g_stations_cn[] = {
    { "中国之声",           "http://lhttp.qingting.fm/live/386/64k.mp3" },
    { "经济之声",           "http://lhttp.qingting.fm/live/387/64k.mp3" },
    { "音乐之声",           "http://lhttp.qingting.fm/live/15318317/64k.mp3" },
    { "北京新闻广播",       "http://lhttp.qingting.fm/live/339/64k.mp3" },
    { "河南交通广播",       "http://lhttp.qingting.fm/live/1209/64k.mp3" },
    { "河南音乐广播",       "http://lhttp.qingting.fm/live/1208/64k.mp3" },
    { "郑州音乐广播",       "http://lhttp.qingting.fm/live/4921/64k.mp3" },
    { "武汉音乐广播",       "http://lhttp.qingting.fm/live/1297/64k.mp3" },
    { "杭州动听968",        "http://lhttp.qingting.fm/live/4866/64k.mp3" },
    { "绍兴交通广播",       "http://lhttp.qingting.fm/live/5053/64k.mp3" },
    { "嘉兴交通广播",       "http://lhttp.qingting.fm/live/1135/64k.mp3" },
    { "湖南摩登音乐台",     "http://lhttp.qingting.fm/live/4980/64k.mp3" },
    { "湖南金鹰955",        "http://lhttp.qingting.fm/live/4522/64k.mp3" },
    { "新疆音乐广播",       "http://lhttp.qingting.fm/live/4029/64k.mp3" },
    { "海峡之声",           "http://lhttp.qingting.fm/live/1746/64k.mp3" },
    { "潮州戏曲广播",       "http://lhttp.qingting.fm/live/4595/64k.mp3" },
};

const int g_stations_cn_count =
    sizeof(g_stations_cn) / sizeof(g_stations_cn[0]);

/* ── American: SomaFM (San Francisco) ───────────────────────────────── */
const radio_station_t g_stations_en[] = {
    { "SomaFM: Groove Salad",    "https://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM: Drone Zone",      "https://ice1.somafm.com/dronezone-128-mp3" },
    { "SomaFM: Secret Agent",    "https://ice1.somafm.com/secretagent-128-mp3" },
    { "SomaFM: Lush",            "https://ice1.somafm.com/lush-128-mp3" },
    { "SomaFM: Indie Pop Rocks", "https://ice1.somafm.com/indiepop-128-mp3" },
    { "SomaFM: DEF CON Radio",   "https://ice1.somafm.com/defcon-128-mp3" },
};

const int g_stations_en_count =
    sizeof(g_stations_en) / sizeof(g_stations_en[0]);
