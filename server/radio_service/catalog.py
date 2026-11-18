"""电台目录模块

从 Radio-Browser 拉取 CN/US 全量电台，清洗、按地区分类、缓存到内存，
供 /api/radio/* 端点使用。后台线程定时刷新。
"""
import threading
import time

import requests

from .provinces import classify_cn, classify_us, REGION_NAME, PROVINCES

RADIO_BROWSER_BASE = "https://de1.api.radio-browser.info"
USER_AGENT = "NomadCast/0.2 (radio catalog; +https://nomadcast.omagine.net)"

_CATALOG: dict[str, dict] = {}   # uuid -> Station
_CATALOG_LOCK = threading.Lock()
_LAST_REFRESH: float = 0.0
_LAST_ERROR: str | None = None

# 公开字段：下发给设备的 station 只保留这些；url 是服务端私有，绝不下发
_PUBLIC_FIELDS = (
    "uuid", "name", "region", "region_name", "codec", "bitrate",
    "hls", "favicon", "votes", "clickcount", "lastcheckok",
)


def _fetch_country(countrycode: str, timeout: int = 60) -> list[dict]:
    """分页拉取某国全部可播电台（hidebroken=true）。"""
    out: list[dict] = []
    offset = 0
    while True:
        url = f"{RADIO_BROWSER_BASE}/json/stations/bycountrycodeexact/{countrycode}"
        resp = requests.get(
            url,
            params={"hidebroken": "true", "limit": 1000, "offset": offset},
            headers={"User-Agent": USER_AGENT},
            timeout=timeout,
        )
        resp.raise_for_status()
        batch = resp.json()
        out.extend(batch)
        if len(batch) < 1000:
            break
        offset += 1000
    return out


def _detect_format(item: dict, url: str) -> tuple[str, str, bool]:
    """返回 (codec, format, is_hls)。codec ∈ MP3|AAC|HLS|UNKNOWN，format ∈ mp3|aac|hls。"""
    low = url.lower()
    if ".m3u8" in low or item.get("hls"):
        return "HLS", "hls", True
    if ".aac" in low:
        return "AAC", "aac", False
    if ".mp3" in low:
        return "MP3", "mp3", False
    codec = (item.get("codec") or "").upper()
    if codec == "MP3":
        return "MP3", "mp3", False
    if codec.startswith("AAC"):
        return "AAC", "aac", False
    # 无法判断：按 mp3 直链处理（最普遍），Content-Type 兜底
    return "UNKNOWN", "mp3", False


def _clean(item: dict) -> dict | None:
    name = (item.get("name") or "").strip()
    uuid = item.get("stationuuid") or item.get("changeuuid")
    url = item.get("url_resolved") or item.get("url") or ""
    if not name or not uuid or not url:
        return None

    country = (item.get("countrycode") or "").upper()
    codec, fmt, is_hls = _detect_format(item, url)

    if country == "CN":
        region_key, region_name = classify_cn(item.get("state", ""), name, item.get("tags", ""))
    else:  # US
        region_key, region_name = classify_us(item.get("state", ""))

    return {
        "uuid": uuid,
        "name": name,
        "country": country,
        "region": region_key,
        "region_name": region_name,
        "codec": codec,
        "format": fmt,
        "hls": is_hls,
        "bitrate": int(item.get("bitrate") or 0),
        "favicon": item.get("favicon") or None,
        "votes": int(item.get("votes") or 0),
        "clickcount": int(item.get("clickcount") or 0),
        "lastcheckok": bool(item.get("lastcheckok")),
        "url": url,   # 私有字段，绝不下发
    }


def _public(s: dict) -> dict:
    return {k: s[k] for k in _PUBLIC_FIELDS}


def refresh() -> None:
    """拉取并重建目录。逐国成功合并，全部失败则保留旧数据。"""
    global _LAST_REFRESH, _LAST_ERROR
    fetched: dict[str, dict[str, dict]] = {}
    errors: list[str] = []

    for code in ("CN", "US"):
        try:
            stations: dict[str, dict] = {}
            for item in _fetch_country(code):
                rec = _clean(item)
                if rec is not None:
                    stations[rec["uuid"]] = rec
            fetched[code] = stations
        except Exception as e:  # noqa: BLE001 — 单国失败不拖垮另一国
            errors.append(f"{code}: {e}")

    if not fetched:
        _LAST_ERROR = "; ".join(errors)
        return

    with _CATALOG_LOCK:
        for code, stations in fetched.items():
            for k in [k for k, v in _CATALOG.items() if v["country"] == code]:
                del _CATALOG[k]
            _CATALOG.update(stations)

    _LAST_REFRESH = time.time()
    _LAST_ERROR = "; ".join(errors) or None


def _playable(country: str) -> list[dict]:
    with _CATALOG_LOCK:
        return [
            s for s in _CATALOG.values()
            if s["country"] == country and s["lastcheckok"]
        ]


def get_countries() -> list[dict]:
    counts: dict[str, int] = {}
    with _CATALOG_LOCK:
        for s in _CATALOG.values():
            if s["lastcheckok"]:
                counts[s["country"]] = counts.get(s["country"], 0) + 1
    return [
        {"code": "CN", "name": "中国", "total": counts.get("CN", 0)},
        {"code": "US", "name": "美国", "total": counts.get("US", 0)},
    ]


def get_regions(country: str) -> list[dict]:
    country = country.upper()
    counts: dict[str, int] = {}
    names: dict[str, str] = {}
    for s in _playable(country):
        r = s["region"]
        counts[r] = counts.get(r, 0) + 1
        names[r] = s["region_name"]

    if country == "CN":
        regions = [{"key": "nationwide", "name": "全国", "order": 0,
                    "count": counts.get("nationwide", 0)}]
        for key, name, order, _alias in PROVINCES:
            regions.append({"key": key, "name": name, "order": order,
                            "count": counts.get(key, 0)})
        regions.append({"key": "other", "name": "其他", "order": 999,
                        "count": counts.get("other", 0)})
        return regions

    # US：按州名字母序，other 置底
    regions = []
    for key in sorted((k for k in counts if k != "other"),
                      key=lambda k: names.get(k, k).lower()):
        regions.append({"key": key, "name": names.get(key, key),
                        "order": len(regions) + 1, "count": counts[key]})
    if "other" in counts:
        regions.append({"key": "other", "name": "其他", "order": 999,
                        "count": counts["other"]})
    return regions


def get_stations(country: str, region: str | None, order: str, q: str | None,
                 offset: int, limit: int) -> tuple[int, list[dict]]:
    country = country.upper()
    ql = (q or "").strip().lower()
    items = [
        s for s in _playable(country)
        if (not region or region == "all" or s["region"] == region)
        and (not ql or ql in s["name"].lower())
    ]

    if order == "name":
        items.sort(key=lambda s: s["name"].lower())
    elif order == "bitrate":
        items.sort(key=lambda s: -s["bitrate"])
    else:  # popular
        items.sort(key=lambda s: -(s["clickcount"] + s["votes"] * 10))

    total = len(items)
    return total, [_public(s) for s in items[offset:offset + limit]]


def get_station(uuid: str) -> dict | None:
    """返回内部完整记录（含私有 url），供 resolve / stream 使用。"""
    with _CATALOG_LOCK:
        return _CATALOG.get(uuid)


def region_display_name(country: str, region: str | None) -> str:
    if not region or region == "all":
        return "全部"
    if region == "nationwide":
        return "全国"
    if region == "other":
        return "其他"
    if country.upper() == "CN":
        return REGION_NAME.get(region, region)
    with _CATALOG_LOCK:
        for s in _CATALOG.values():
            if s["country"] == country and s["region"] == region:
                return s["region_name"]
    return region
