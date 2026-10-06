"""iTunes Lookup API

通过播客 ID 获取完整信息，包括 feedUrl。
排行榜/分类接口只返回 id，需用此接口补全 RSS 地址。

Apple 的 Lookup API 有速率限制，而播客元数据（feedUrl/trackCount/artwork）变化
很慢，所以这里加了一层进程内 TTL 缓存：单集查询和批量查询共享同一份 per-id 缓存，
批量只对未命中的 id 发起一次上游请求。查询不到的 id 也会做短时负缓存，避免反复
打上游。
"""
import threading
import time
import requests

LOOKUP_URL = "https://itunes.apple.com/lookup"

# ── 进程内 TTL 缓存 ──────────────────────────────────────────────────────────
_CACHE_TTL_SECONDS = 3600          # 1 小时 — 播客元数据变化很慢
_NEGATIVE_TTL_SECONDS = 300        # 5 分钟 — 不存在的 id
_CACHE_MAX_ENTRIES = 512

_cache: dict[tuple, tuple] = {}    # (country, id) -> (expires_monotonic, value)
_cache_lock = threading.Lock()
_NOT_FOUND = object()              # 负缓存哨兵


def _cache_get(key):
    """Return the cached value; None on miss. A negative hit returns _NOT_FOUND."""
    with _cache_lock:
        entry = _cache.get(key)
        if entry is None:
            return None
        expires, value = entry
        if expires <= time.monotonic():
            del _cache[key]
            return None
        return value


def _cache_put(key, value, ttl):
    with _cache_lock:
        _cache[key] = (time.monotonic() + ttl, value)
        # 简单 LRU：超上限时淘汰最早过期的条目
        while len(_cache) > _CACHE_MAX_ENTRIES:
            oldest = min(_cache, key=lambda k: _cache[k][0])
            del _cache[oldest]


def _get_json(params: dict, timeout: int) -> dict:
    """GET the Lookup API with a short retry loop. Raises on persistent failure."""
    last_err = None
    for _ in range(3):
        try:
            resp = requests.get(LOOKUP_URL, params=params, timeout=timeout)
            resp.raise_for_status()
            return resp.json()
        except Exception as e:
            last_err = e
            time.sleep(0.5)
    raise last_err


def _make_record(item: dict) -> dict:
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


def lookup_podcast(
    collection_id: int | str,
    country: str = "cn",
    timeout: int = 10,
) -> dict | None:
    """
    按播客 ID 查询完整信息。

    Returns:
        播客信息 dict；未找到时返回 None。
    """
    cid = int(collection_id)
    key = (country, cid)
    cached = _cache_get(key)
    if cached is _NOT_FOUND:
        return None
    if cached is not None:
        return cached

    data = _get_json({"id": cid, "country": country}, timeout)
    results = data.get("results", [])
    if not results:
        _cache_put(key, _NOT_FOUND, _NEGATIVE_TTL_SECONDS)
        return None

    rec = _make_record(results[0])
    _cache_put(key, rec, _CACHE_TTL_SECONDS)
    return rec


def batch_lookup_podcasts(
    collection_ids: list[int | str],
    country: str = "cn",
    timeout: int = 15,
) -> list[dict]:
    """
    批量查询播客信息（最多 50 个 ID）。

    命中缓存的部分直接复用，只对缺失的 id 发起一次上游请求。
    注意：如果某个 id 不存在，返回结果中会缺少该项，不会报错（并做负缓存）。
    """
    if not collection_ids:
        return []

    ids = list(dict.fromkeys(int(cid) for cid in collection_ids[:50]))

    result_map: dict[int, dict] = {}
    missing: list[int] = []
    for cid in ids:
        cached = _cache_get((country, cid))
        if cached is _NOT_FOUND:
            continue
        if cached is not None:
            result_map[cid] = cached
        else:
            missing.append(cid)

    if missing:
        ids_str = ",".join(str(cid) for cid in missing)
        data = _get_json({"id": ids_str, "country": country}, timeout)
        fetched = {
            int(item.get("collectionId")): item
            for item in data.get("results", [])
            if item.get("collectionId")
        }
        for cid in missing:
            item = fetched.get(cid)
            if item is None:
                _cache_put((country, cid), _NOT_FOUND, _NEGATIVE_TTL_SECONDS)
            else:
                rec = _make_record(item)
                _cache_put((country, cid), rec, _CACHE_TTL_SECONDS)
                result_map[cid] = rec

    return [result_map[cid] for cid in ids if cid in result_map]
