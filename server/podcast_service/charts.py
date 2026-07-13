"""Apple Podcasts 排行榜

通过 Apple RSS Feed Generator API 获取各国家/分类的热门播客榜单。
榜单只返回 id，不含 feedUrl，需搭配 lookup 接口获取完整信息。
"""
import requests

# 新版 API（简洁 JSON，推荐）
RSS_FEED_GENERATOR = "https://rss.applemarketingtools.com/api/v2"

# 旧版 iTunes RSS（按 genre 过滤时用）
ITUNES_RSS = "https://itunes.apple.com"


def get_top_podcasts(
    country: str = "cn",
    limit: int = 25,
    genre_id: int | None = None,
    timeout: int = 10,
) -> dict:
    """
    获取热门播客排行榜。

    Args:
        country: 国家代码 (cn, us, jp, ...)
        limit:  返回数量 (最大 200)
        genre_id: 分类 ID，None 表示不按分类过滤
        timeout: 请求超时秒数

    Returns:
        {
            "title": "热门节目",
            "country": "cn",
            "updated": "Fri, 26 Jun 2026 06:25:07 +0000",
            "results": [
                {
                    "id": "1582119137",
                    "name": "岩中花述",
                    "artist_name": "GIADA",
                    "genres": [{"genre_id": "1301", "name": "艺术"}],
                    "artwork_url": "https://...",
                    "apple_url": "https://podcasts.apple.com/cn/.../id1582119137"
                },
                ...
            ]
        }
    """
    if genre_id:
        # 带分类的榜单用新版 API 的 top-podcasts 端点
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top-podcasts/{genre_id}/{limit}/explicit.json"
    else:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top/{limit}/podcasts.json"

    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            break
        except Exception as e:
            last_err = e
            import time; time.sleep(0.5)
    else:
        raise last_err

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

    Args:
        country: 国家代码
        limit:  返回数量
        genre_id: 分类 ID
        timeout: 请求超时秒数

    Returns:
        {
            "title": "热门单集",
            "country": "cn",
            "results": [
                {
                    "episode_name": "...",
                    "podcast_name": "...",
                    "podcast_id": "...",
                    "audio_preview_url": "...",  // 可能有预览片段
                    "artwork_url": "...",
                }
            ]
        }
    """
    if genre_id:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top-podcast-episodes/{genre_id}/{limit}/explicit.json"
    else:
        url = f"{RSS_FEED_GENERATOR}/{country}/podcasts/top/{limit}/podcast-episodes.json"

    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            break
        except Exception as e:
            last_err = e
            import time; time.sleep(0.5)
    else:
        raise last_err

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
