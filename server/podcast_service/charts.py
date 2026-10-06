"""Apple Podcasts 排行榜

通过 Apple RSS Feed Generator API 获取各国家/分类的热门播客榜单。
榜单只返回 id，不含 feedUrl，需搭配 lookup 接口获取完整信息。

榜单更新不频繁，且端侧首页每次进入都会拉 `/api/charts/full`，所以这里按 URL 做
进程内 TTL 缓存，避免对上游重复请求。
"""
import threading
import time
import requests

# 新版 API（简洁 JSON，推荐）
RSS_FEED_GENERATOR = "https://rss.applemarketingtools.com/api/v2"

# 旧版 iTunes RSS（按 genre 过滤时用）
ITUNES_RSS = "https://itunes.apple.com"

# ── 进程内 TTL 缓存 ──────────────────────────────────────────────────────────
_CACHE_TTL_SECONDS = 600           # 10 分钟 — 榜单更新不频繁
_CACHE_MAX_ENTRIES = 128

_cache: dict[str, tuple] = {}      # url -> (expires_monotonic, json)
_cache_lock = threading.Lock()


def _get_json(url: str, timeout: int) -> dict:
    """GET with a short retry loop. Raises on persistent failure."""
    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            return resp.json()
        except Exception as e:
            last_err = e
            time.sleep(0.5)
    raise last_err


def _cached(url: str, timeout: int) -> dict:
    with _cache_lock:
        entry = _cache.get(url)
        if entry is not None:
            expires, data = entry
            if expires > time.monotonic():
                return data
            del _cache[url]

    data = _get_json(url, timeout)
    with _cache_lock:
        _cache[url] = (time.monotonic() + _CACHE_TTL_SECONDS, data)
        while len(_cache) > _CACHE_MAX_ENTRIES:
            oldest = min(_cache, key=lambda k: _cache[k][0])
            del _cache[oldest]
    return data


def get_top_podcasts(
    country: str = "cn",
    limit: int = 25,
    genre_id: int | None = None,
    timeout: int = 10,
) -> dict:
    """
    获取热门播客排行榜。

    Returns:
        {
            "title": "热门节目",
            "country": "cn",
            "updated": "...",
            "results": [ { "id", "name", "artist_name", "genres", "artwork_url", "apple_url", ... } ],
            "total": N,
        }
    """
    if genre_id:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top-podcasts/{genre_id}/{limit}/explicit.json"
    else:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top/{limit}/podcasts.json"

    data = _cached(url, timeout)

    feed = data.get("feed", {})
    results = []
    for item in feed.get("results", []):
        results.append({
            "id": item.get("id", ""),
            "name": item.get("name", ""),
            "artist_name": item.get("artistName", ""),
            "genres": [
                {"genre_id": g.get("genreId", ""), "name": g.get("name", "")}
                for g in item.get("genres", [])
            ],
            "artwork_url": item.get("artworkUrl100", ""),
            "apple_url": item.get("url", ""),
            "content_rating": item.get("contentAdvisoryRating", ""),
        })

    return {
        "title": feed.get("title", ""),
        "country": feed.get("country", country),
        "updated": feed.get("updated", ""),
        "results": results,
        "total": len(results),
    }


def get_top_episodes(
    country: str = "cn",
    limit: int = 25,
    genre_id: int | None = None,
    timeout: int = 10,
) -> dict:
    """
    获取热门单集排行榜。

    Returns:
        {
            "title": "热门单集",
            "country": "cn",
            "results": [ { "episode_name", "podcast_name", "podcast_id", "episode_guid", ... } ],
            "total": N,
        }
    """
    if genre_id:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top-podcast-episodes/{genre_id}/{limit}/explicit.json"
    else:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top/{limit}/podcast-episodes.json"

    data = _cached(url, timeout)

    feed = data.get("feed", {})
    results = []
    for item in feed.get("results", []):
        results.append({
            "episode_name": item.get("name", ""),
            "podcast_name": item.get("artistName", ""),
            "podcast_id": item.get("collectionId", ""),
            "episode_guid": item.get("id", ""),
            "artwork_url": item.get("artworkUrl100", ""),
            "apple_url": item.get("url", ""),
            "duration_ms": item.get("duration", 0),
        })

    return {
        "title": feed.get("title", ""),
        "country": feed.get("country", country),
        "updated": feed.get("updated", ""),
        "results": results,
        "total": len(results),
    }
