"""播客 RSS Feed 解析

使用 feedparser 做常规解析，同时用 ElementTree 直接解析 XML 提取完整 enclosure URL，
防止 feedparser 对特殊 URL 格式（如 xyzfm.space 的 media.xyzcdn.net 路径）做截断。

翻页优化：同一 feed_url 的完整解析结果按 TTL 缓存在内存里。翻页时服务端无需
重复下载 + 解析整个 RSS，只需从缓存切片，把每次请求从秒级降到毫秒级。
"""
import threading
import time
import xml.etree.ElementTree as ET
import requests
import feedparser

# ── 内存 TTL 缓存 ──────────────────────────────────────────────────────────
_CACHE_TTL_SECONDS = 900        # 15 分钟 — 播客 RSS 更新不频繁
_CACHE_MAX_ENTRIES = 64

_cache: dict[str, dict] = {}
_cache_lock = threading.Lock()


def _extract_enclosure_urls_from_xml(raw_xml: bytes) -> dict[str, str]:
    """
    从原始 XML 中用 ElementTree 直接提取 <enclosure url="..."> 的完整 URL。

    feedparser 对 xyzfm 系 URL（内含 media.xyzcdn.net/.../file.m4a）会错误截断，
    因此用此函数做兜底。

    Returns:
        {guid: full_enclosure_url} 的映射
    """
    enclosure_map: dict[str, str] = {}
    try:
        root = ET.fromstring(raw_xml)
        channel = root.find("channel")
        if channel is None:
            return enclosure_map
        for item in channel.findall("item"):
            guid_el = item.find("guid")
            guid = guid_el.text if guid_el is not None else ""
            enc = item.find("enclosure")
            if enc is not None:
                url = enc.get("url", "")
                if url and guid:
                    enclosure_map[guid] = url
    except ET.ParseError:
        pass
    return enclosure_map


def _build_full_feed(raw_xml: bytes, feed) -> dict:
    """把 feedparser 结果 + 原始 XML 组装成完整 feed（不切片）。"""
    xml_enclosure_map = _extract_enclosure_urls_from_xml(raw_xml)

    episodes = []
    for entry in feed.entries:
        audio_url = ""
        guid = entry.get("id", "")
        if guid and guid in xml_enclosure_map:
            audio_url = xml_enclosure_map[guid]

        if not audio_url:
            for link in entry.get("links", []):
                if link.get("rel") == "enclosure":
                    audio_url = link.get("href", "")
                    break

        if not audio_url:
            for link in entry.get("links", []):
                href = link.get("href", "")
                if href and any(
                    href.lower().endswith(ext)
                    for ext in (".mp3", ".m4a", ".mp4")
                ):
                    audio_url = href
                    break

        episodes.append({
            "title": entry.get("title", ""),
            "audio_url": audio_url,
            "published": entry.get("published", ""),
            "duration": entry.get("itunes_duration", ""),
        })

    return {
        "title": feed.feed.get("title", ""),
        "description": feed.feed.get("description", ""),
        "image_url": feed.feed.get("image", {}).get("href", ""),
        "link": feed.feed.get("link", ""),
        "total": len(feed.entries),
        "episodes": episodes,
    }


def parse_feed(feed_url: str, timeout: int = 15, offset: int = 0, limit: int = 50) -> dict:
    """
    下载并解析播客 RSS feed（带 TTL 缓存）。

    Args:
        feed_url: RSS feed URL（来自苹果搜索结果中的 feedUrl）
        timeout: 请求超时秒数
        offset: 分页偏移（0-indexed）
        limit: 每页最多返回数（默认 50）

    Returns:
        {
            "title": "播客名",
            "description": "播客简介",
            "image_url": "封面图URL",
            "link": "播客主页",
            "total": 30,          # RSS 中的总单集数
            "episodes": [
                {
                    "title": "第X期标题",
                    "audio_url": "https://...mp3",
                    "published": "Thu, 25 Jun 2026 12:00:00 GMT",
                    "duration": "30:15",
                },
                ...
            ]
        }
    """
    now = time.monotonic()

    # 1. 命中未过期缓存 → 直接复用完整结果；否则下载 + 解析 + 写缓存
    with _cache_lock:
        hit = _cache.get(feed_url)
    if hit and (now - hit["at"]) < _CACHE_TTL_SECONDS:
        full = hit["data"]
    else:
        resp = requests.get(feed_url, timeout=timeout)
        resp.raise_for_status()
        full = _build_full_feed(resp.content, feedparser.parse(resp.content))

        if not full["episodes"]:
            raise ValueError("Content unavailable — this feed may not be a valid RSS source")

        with _cache_lock:
            _cache[feed_url] = {"at": now, "data": full}
            # 简单 LRU：超上限时淘汰最旧条目
            while len(_cache) > _CACHE_MAX_ENTRIES:
                oldest = min(_cache, key=lambda k: _cache[k]["at"])
                del _cache[oldest]

    # 2. 从完整结果切片
    page = full["episodes"][offset:offset + limit]
    if not page:
        raise ValueError("Content unavailable — this feed may not be a valid RSS source")

    return {
        "title": full["title"],
        "description": full["description"],
        "image_url": full["image_url"],
        "link": full["link"],
        "total": full["total"],
        "episodes": page,
    }
