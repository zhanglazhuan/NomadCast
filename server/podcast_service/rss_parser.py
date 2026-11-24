"""播客 RSS Feed 解析

使用 feedparser 做常规解析，同时用 ElementTree 直接解析 XML 提取完整 enclosure URL，
防止 feedparser 对特殊 URL 格式（如 xyzfm.space 的 media.xyzcdn.net 路径）做截断。

翻页优化：同一 feed_url 的完整解析结果按 TTL 缓存在内存里。翻页时服务端无需
重复下载 + 解析整个 RSS，只需从缓存切片，把每次请求从秒级降到毫秒级。
"""
import re
import threading
import time
import xml.etree.ElementTree as ET
import requests
import feedparser

# 播客源站（尤其小宇宙 feed.xyzfm.space）会对无浏览器特征的请求做反爬拦截，
# 用浏览器 UA + 明确的 Accept 声明再请求，能显著降低被误伤的概率。
_FEED_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/120.0.0.0 Safari/537.36"),
    "Accept": "application/rss+xml, application/xml, text/xml, */*;q=0.8",
    "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
}

# HTML 拦截页特征 → 人类可读原因。feedparser 对任意字节都能“吞下去”返回 0 条，
# 之前会误报成 “not a valid RSS source”。这里识别真实失败原因，给出可操作的提示。
_BLOCK_SIGNATURES = [
    (r"无法访问|訪問受限|访问受限|暫時無法|暂时无法", "源站 CDN 屏蔽（地区/IP 限制，常见于小宇宙 feed.xyzfm.space）"),
    (r"403", "HTTP 403 — 源站拒绝访问（CDN/地区/IP 限制或反爬拦截）"),
    (r"FORBIDDEN", "HTTP 403 FORBIDDEN — 源站拒绝访问"),
]


def _looks_like_html(content: bytes) -> bool:
    """粗略判断响应体是网页而非 XML/RSS。"""
    head = content[:2000].lower()
    return head.lstrip().startswith(b"<!doctype html") or head.lstrip().startswith(b"<html") \
        or b"<html" in head


def _html_block_reason(content: bytes) -> str | None:
    """从 HTML 拦截页里识别出具体原因；识别不出返回 None。"""
    text = content.decode("utf-8", errors="ignore")
    for pattern, reason in _BLOCK_SIGNATURES:
        if re.search(pattern, text, re.IGNORECASE):
            return reason
    return None


# ── 内存 TTL 缓存 ──────────────────────────────────────────────────────────
_CACHE_TTL_SECONDS = 900        # 15 分钟 — 播客 RSS 更新不频繁（软过期，过期即重新拉取）
_CACHE_STALE_SECONDS = 24 * 3600  # 24 小时 — 兜底窗口：上游全挂时最多返回这么久前的旧数据
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


def _download_and_parse(feed_url: str, timeout: int) -> dict:
    """下载 feed 并解析成完整结果（不切片）。失败抛 ValueError / requests 异常。"""
    resp = requests.get(feed_url, headers=_FEED_HEADERS, timeout=timeout)
    # 源站可能用 HTTP 状态码（403/429）直接拒绝，也可能返回 200 但 body 是
    # 拦截网页。两种情况都给出清晰原因，而不是含糊的 "not a valid RSS source"。
    if resp.status_code in (403, 429):
        raise ValueError(
            f"源站拒绝访问 (HTTP {resp.status_code})，可能是地区/IP 限制或反爬拦截"
        )
    resp.raise_for_status()

    content = resp.content
    if _looks_like_html(content):
        reason = _html_block_reason(content)
        raise ValueError(
            reason or "源站返回了网页而非 RSS/XML（该地址可能已失效或需要鉴权）"
        )

    full = _build_full_feed(content, feedparser.parse(content))

    if not full["episodes"]:
        raise ValueError("该 feed 里没有可解析的单集（可能是空 feed 或格式不受支持）")

    return full


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
    stale = False

    # 1. 命中未过期缓存 → 直接复用完整结果；否则下载 + 解析 + 写缓存。
    #    上游失败时兜底读缓存（即使已过期，只要在 stale 窗口内就返回旧数据）。
    with _cache_lock:
        hit = _cache.get(feed_url)
    if hit and (now - hit["at"]) < _CACHE_TTL_SECONDS:
        full = hit["data"]
    else:
        try:
            full = _download_and_parse(feed_url, timeout)
        except Exception:
            # 上游失败 → 兜底读缓存（stale 窗口内返回旧数据，而不是让设备看到 502）。
            with _cache_lock:
                stale_hit = _cache.get(feed_url)
            if stale_hit and (now - stale_hit["at"]) < _CACHE_STALE_SECONDS:
                full = stale_hit["data"]
                stale = True
            else:
                raise
        else:
            with _cache_lock:
                _cache[feed_url] = {"at": now, "data": full}
                # 简单 LRU：超上限时淘汰最旧条目
                while len(_cache) > _CACHE_MAX_ENTRIES:
                    oldest = min(_cache, key=lambda k: _cache[k]["at"])
                    del _cache[oldest]

    # 2. 从完整结果切片。offset 超出范围时给空列表而非报错——翻页到底不是失败，
    #    更不该触发黑名单；与 itunes_feed.fetch_episodes 的行为保持一致。
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
