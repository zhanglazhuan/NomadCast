"""小宇宙（xyzfm）播客的单集兜底获取 — 通过 iTunes Lookup 绕过被 IP 屏蔽的 RSS。

背景：feed.xyzfm.space（小宇宙的官方 RSS 域名）现在对非白名单 IP 直接返回 403
（`X-Tengine-Error: denied by IP ACL = not in whitelist`），浏览器 UA / Accept 头都
无法绕过（这是 IP 级 ACL，不是反爬 UA 检测），于是 parse_feed 只能拿到拦截页。

但苹果的 iTunes Lookup API 会抓取并缓存小宇宙的 feed，`lookup?id=<collection_id>
&entity=podcastEpisode` 能直接拿到单集列表，且每个单集的 `episodeUrl` 里嵌着
media.xyzcdn.net 的音频直链（绕开被屏蔽的 feed 域名）。因此用这个接口做兜底。

单集总数：iTunes Lookup 对单集列表硬性截断在最近 200 集，但小宇宙 web 页的
`__NEXT_DATA__` 里有真实的 `episodeCount`（如姜思达 314 集）。这里优先用真实总数
做 total，让客户端显示「314 集」而非被截断的 200；拿不到真实总数时退化为 iTunes
trackCount → 实际条数。
"""
import json as _json
import re
import threading
import time

import requests

LOOKUP_URL = "https://itunes.apple.com/lookup"
WEB_PODCAST_URL = "https://www.xiaoyuzhoufm.com/podcast/{pid}"
_MAX_EPISODES = 200   # Apple Lookup 对单集列表的硬上限（更高的 limit 会被截断）

_WEB_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/120.0.0.0 Safari/537.36"),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
}

# ── TTL 缓存（同 rss_parser 的模式）：按 collection_id 缓存完整结果，翻页只切片 ──
_CACHE_TTL_SECONDS = 900        # 15 分钟 — 单集列表更新不频繁（软过期，过期即重新拉取）
_CACHE_STALE_SECONDS = 24 * 3600  # 24 小时 — 兜底窗口：上游全挂时最多返回这么久前的旧数据
_CACHE_MAX_ENTRIES = 64

_cache: dict[int, dict] = {}
_cache_lock = threading.Lock()


def is_xyzfm_feed(feed_url: str) -> bool:
    """判断 feed URL 是否指向小宇宙（这些 feed 的 RSS 源站已被 IP ACL 屏蔽）。"""
    url = (feed_url or "").lower()
    return "xyzfm" in url or "xiaoyuzhoufm.com" in url


def _extract_pid(episode_url: str) -> str | None:
    """从 episodeUrl 里抽出小宇宙 podcast pid（24 位 hex ObjectId）。

    episodeUrl 形如 https://dts-api.xiaoyuzhoufm.com/track/<pid>/<eid>/media.xyzcdn.net/...
    """
    m = re.search(r"/track/([0-9a-fA-F]{24})/", episode_url or "")
    return m.group(1) if m else None


def _audio_url_from_episode(episode: dict) -> str:
    """从 iTunes 的 episodeUrl 里抽出可直接下载的 media.xyzcdn.net 直链。

    episodeUrl 形如:
        https://dts-api.xiaoyuzhoufm.com/track/<pid>/<eid>/media.xyzcdn.net/<pid>/<file>.m4a
    后半段 media.xyzcdn.net/... 才是真正可下载的媒体地址；dts-api 只是 302 转发层，
    直接用它既少一跳跳转，又复用 downloader 里已有的 xyzcdn headers 逻辑。
    """
    url = episode.get("episodeUrl", "") or ""
    marker = "media.xyzcdn.net"
    idx = url.find(marker)
    if idx >= 0:
        return "https://" + url[idx:]
    return url


def _duration_str(track_time_millis) -> str:
    """毫秒 → 秒字符串。客户端用 atoi 把无冒号字符串当作秒数解析。"""
    try:
        ms = int(track_time_millis or 0)
    except (TypeError, ValueError):
        return ""
    if ms <= 0:
        return ""
    return str(ms // 1000)


def _fetch_true_total(pid: str, timeout: int) -> int | None:
    """从小宇宙 web 页的 __NEXT_DATA__ 里拿真实集数 episodeCount。

    Best-effort：web 页打不开 / 解析失败都返回 None，由调用方逐级退化。
    """
    try:
        resp = requests.get(WEB_PODCAST_URL.format(pid=pid), headers=_WEB_HEADERS, timeout=timeout)
        resp.raise_for_status()
        m = re.search(
            rb'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>',
            resp.content, re.S,
        )
        if not m:
            return None
        data = _json.loads(m.group(1))
        return data.get("props", {}).get("pageProps", {}).get("podcast", {}).get("episodeCount")
    except Exception:
        return None


def _load_full(collection_id: int, timeout: int) -> dict:
    """iTunes Lookup 拉完整单集列表 + 小宇宙 web 页拿真实总数。带 TTL 缓存。"""
    with _cache_lock:
        hit = _cache.get(collection_id)
    if hit and (time.monotonic() - hit["at"]) < _CACHE_TTL_SECONDS:
        return hit["data"]

    params = {
        "id": collection_id,
        "entity": "podcastEpisode",
        "limit": _MAX_EPISODES,
        "country": "cn",
    }
    resp = requests.get(LOOKUP_URL, params=params, timeout=timeout)
    resp.raise_for_status()
    data = resp.json()

    podcast = None
    episodes_raw = []
    for item in data.get("results", []):
        wt = item.get("wrapperType")
        if wt == "track":
            podcast = item
        elif wt == "podcastEpisode":
            episodes_raw.append(item)

    pid = None
    for ep in episodes_raw:
        pid = _extract_pid(ep.get("episodeUrl", ""))
        if pid:
            break

    episodes = []
    for ep in episodes_raw:
        audio_url = _audio_url_from_episode(ep)
        if not audio_url:
            continue
        episodes.append({
            "title": ep.get("trackName", ""),
            "audio_url": audio_url,
            "published": ep.get("releaseDate", ""),
            "duration": _duration_str(ep.get("trackTimeMillis")),
        })

    if not episodes:
        raise ValueError("iTunes 未能返回该播客的单集（可能尚未被 Apple 收录）")

    # 真实总数：小宇宙 web 页 episodeCount（最准）→ iTunes trackCount → 实际条数
    true_total = _fetch_true_total(pid, timeout) if pid else None
    if not true_total:
        true_total = (podcast or {}).get("trackCount")
    if not true_total:
        true_total = len(episodes)

    full = {
        "title": (podcast or {}).get("collectionName", ""),
        "description": "",
        "image_url": (podcast or {}).get("artworkUrl600")
                     or (podcast or {}).get("artworkUrl100", ""),
        "link": (podcast or {}).get("collectionViewUrl", ""),
        "total": int(true_total),
        "episodes": episodes,
    }

    with _cache_lock:
        _cache[collection_id] = {"at": time.monotonic(), "data": full}
        # 简单 LRU：超上限时淘汰最旧条目
        while len(_cache) > _CACHE_MAX_ENTRIES:
            oldest = min(_cache, key=lambda k: _cache[k]["at"])
            del _cache[oldest]

    return full


def _load_cached(collection_id: int) -> dict | None:
    """兜底读缓存：即使已软过期，只要在 stale 窗口内就返回旧数据。

    用于上游（iTunes / 小宇宙 web 页）请求失败时降级——量产场景下「能听旧的」
    永远比「报错」稳。返回 None 表示连旧数据都没有（冷启动或超出 stale 窗口）。
    """
    with _cache_lock:
        hit = _cache.get(collection_id)
    if not hit:
        return None
    if (time.monotonic() - hit["at"]) < _CACHE_STALE_SECONDS:
        return hit["data"]
    return None


def fetch_episodes(collection_id, offset: int = 0, limit: int = 50, timeout: int = 15) -> dict:
    """通过 iTunes Lookup 获取某播客的单集列表。

    返回结构与 parse_feed 一致（title/description/image_url/link/total/episodes）。
    total 是真实总数（可能 > 实际能返回的 200 条），offset 超出可返回范围时给空列表
    而非报错，这样客户端翻到很后面的页只是空页，不会触发 502/黑名单。

    Raises:
        ValueError: 上游未返回任何单集（播客可能未被 Apple 收录，或参数有误）。
    """
    stale = False
    try:
        full = _load_full(int(collection_id), timeout)
    except Exception:
        # 上游失败 → 兜底读缓存（stale 窗口内返回旧数据，而不是让设备看到 502）。
        full = _load_cached(int(collection_id))
        if full is None:
            raise
        stale = True

    page = full["episodes"][offset:offset + limit]

    result = {
        "title": full["title"],
        "description": full["description"],
        "image_url": full["image_url"],
        "link": full["link"],
        "total": full["total"],
        "episodes": page,
    }
    if stale:
        result["stale"] = True
    return result
