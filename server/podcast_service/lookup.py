"""iTunes Lookup API

通过播客 ID 获取完整信息，包括 feedUrl。
排行榜/分类接口只返回 id，需用此接口补全 RSS 地址。
"""
import requests

LOOKUP_URL = "https://itunes.apple.com/lookup"


def lookup_podcast(
    collection_id: int | str,
    country: str = "cn",
    timeout: int = 10,
) -> dict:
    """
    按播客 ID 查询完整信息。

    Args:
        collection_id: 播客 collectionId（来自排行榜/搜索结果的 id 字段）
        country: 国家代码
        timeout: 请求超时秒数

    Returns:
        {
            "collection_id": 1256399960,
            "collection_name": "故事FM",
            "artist_name": "寇爱哲",
            "feed_url": "https://feeds.storyfm.cn/storyfm.xml",
            "artwork_url": "https://...",
            "genres": ["Society & Culture", "Podcasts"],
            "track_count": 975,
            "apple_url": "https://podcasts.apple.com/cn/...",
        }
        未找到时返回 None
    """
    params = {
        "id": int(collection_id),
        "country": country,
    }
    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(LOOKUP_URL, params=params, timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            break
        except Exception as e:
            last_err = e
            import time; time.sleep(0.5)
    else:
        raise last_err

    results = data.get("results", [])
    if not results:
        return None

    item = results[0]
    return {
        "collection_id": item.get("collectionId"),
        "collection_name": item.get("collectionName", ""),
        "artist_name": item.get("artistName", ""),
        "feed_url": item.get("feedUrl", ""),
        "artwork_url": item.get("artworkUrl600", item.get("artworkUrl100", "")),
        "genres": item.get("genres", []),
        "genre_ids": item.get("genreIds", []),
        "track_count": item.get("trackCount", 0),
        "apple_url": item.get("collectionViewUrl", ""),
        "release_date": item.get("releaseDate", ""),
    }


def batch_lookup_podcasts(
    collection_ids: list[int | str],
    country: str = "cn",
    timeout: int = 15,
) -> list[dict]:
    """
    批量查询播客信息（最多 50 个 ID）。

    iTunes Lookup API 支持逗号分隔的多个 id，一次请求返回所有结果。
    注意：如果某个 id 不存在，返回结果中会缺少该项，不会报错。

    Args:
        collection_ids: 播客 ID 列表（最多 50）
        country: 国家代码
        timeout: 请求超时秒数

    Returns:
        播客信息列表，长度可能少于输入（不存在的 id 被跳过）
    """
    if not collection_ids:
        return []

    ids_str = ",".join(str(int(cid)) for cid in collection_ids[:50])
    params = {
        "id": ids_str,
        "country": country,
    }
    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(LOOKUP_URL, params=params, timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            break
        except Exception as e:
            last_err = e
            import time; time.sleep(0.5)
    else:
        raise last_err

    results = []
    for item in data.get("results", []):
        results.append({
            "collection_id": item.get("collectionId"),
            "collection_name": item.get("collectionName", ""),
            "artist_name": item.get("artistName", ""),
            "feed_url": item.get("feedUrl", ""),
            "artwork_url": item.get("artworkUrl600", item.get("artworkUrl100", "")),
            "genres": item.get("genres", []),
            "genre_ids": item.get("genreIds", []),
            "track_count": item.get("trackCount", 0),
            "apple_url": item.get("collectionViewUrl", ""),
            "release_date": item.get("releaseDate", ""),
        })
    return results
