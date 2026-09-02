"""Apple Podcasts Search API 封装"""
import requests

ITUNES_SEARCH_URL = "https://itunes.apple.com/search"


def search_podcasts(
    keyword: str,
    country: str = "cn",
    limit: int = 10,
    timeout: int = 10,
) -> list[dict]:
    """
    搜索播客专辑。

    Args:
        keyword: 搜索关键词
        country: 国家代码，默认 cn（中国区优先）
        limit: 返回结果数量上限

    Returns:
        播客列表，每项包含 collectionName, feedUrl, artworkUrl100, artistName 等
    """
    params = {
        "term": keyword,
        "media": "podcast",
        "entity": "podcast",
        "country": country,
        "limit": limit,
    }
    resp = requests.get(ITUNES_SEARCH_URL, params=params, timeout=timeout)
    resp.raise_for_status()
    data = resp.json()

    results = []
    for item in data.get("results", []):
        results.append({
            "collection_id": item.get("collectionId"),
            "collection_name": item.get("collectionName", ""),
            "artist_name": item.get("artistName", ""),
            "feed_url": item.get("feedUrl", ""),
            "artwork_url": item.get("artworkUrl100", ""),
            "genre": item.get("primaryGenreName", ""),
            "track_count": item.get("trackCount", 0),
        })
    return results


def search_episodes(
    keyword: str,
    country: str = "cn",
    limit: int = 10,
    timeout: int = 10,
) -> list[dict]:
    """
    搜索播客单集。

    Args:
        keyword: 搜索关键词
        country: 国家代码，默认 cn
        limit: 返回结果数量上限

    Returns:
        单集列表，每项包含 episode 标题、所属播客、播放地址、时长等
    """
    params = {
        "term": keyword,
        "media": "podcast",
        "entity": "podcastEpisode",
        "country": country,
        "limit": limit,
    }
    resp = requests.get(ITUNES_SEARCH_URL, params=params, timeout=timeout)
    resp.raise_for_status()
    data = resp.json()

    results = []
    for item in data.get("results", []):
        results.append({
            "track_id": item.get("trackId"),
            "episode_title": item.get("trackName", ""),
            "collection_id": item.get("collectionId"),
            "collection_name": item.get("collectionName", ""),
            "feed_url": item.get("feedUrl", ""),
            "artist_name": item.get("artistName", ""),
            "audio_preview_url": item.get("previewUrl", ""),
            "artwork_url": item.get("artworkUrl160", ""),
            "duration_ms": item.get("trackTimeMillis", 0),
            "release_date": item.get("releaseDate", ""),
            "description": item.get("description", ""),
            "episode_guid": item.get("episodeGuid", ""),
        })
    return results
