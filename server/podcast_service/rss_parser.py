"""播客 RSS Feed 解析

使用 feedparser 做常规解析，同时用 ElementTree 直接解析 XML 提取完整 enclosure URL，
防止 feedparser 对特殊 URL 格式（如 xyzfm.space 的 media.xyzcdn.net 路径）做截断。
"""
import xml.etree.ElementTree as ET
import requests
import feedparser


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


def parse_feed(feed_url: str, timeout: int = 15, offset: int = 0, limit: int = 50) -> dict:
    """
    下载并解析播客 RSS feed。

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
                    "description": "单集简介",
                },
                ...
            ]
        }
    """
    resp = requests.get(feed_url, timeout=timeout)
    resp.raise_for_status()
    raw_xml = resp.content
    feed = feedparser.parse(raw_xml)

    # ElementTree 兜底提取完整 enclosure URL（修复 xyzfm URL 截断问题）
    xml_enclosure_map = _extract_enclosure_urls_from_xml(raw_xml)

    total_entries = len(feed.entries)

    podcast_info = {
        "title": feed.feed.get("title", ""),
        "description": feed.feed.get("description", ""),
        "image_url": feed.feed.get("image", {}).get("href", ""),
        "link": feed.feed.get("link", ""),
        "total": total_entries,
        "episodes": [],
    }

    # Slice for pagination
    page = feed.entries[offset:offset + limit]
    for entry in page:
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

        # Only return essential fields — ESP32 has limited RAM.
        # Description (often 5KB+ HTML) is NOT sent; ESP32 can fetch
        # it on demand when the user opens the episode detail.
        episode = {
            "title": entry.get("title", ""),
            "audio_url": audio_url,
            "published": entry.get("published", ""),
            "duration": entry.get("itunes_duration", ""),
        }
        podcast_info["episodes"].append(episode)

    if not podcast_info["episodes"]:
        raise ValueError("Content unavailable — this feed may not be a valid RSS source")

    return podcast_info
