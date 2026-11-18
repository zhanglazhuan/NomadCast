"""电台流转发

统一走服务器代理。MP3/AAC 直接字节透传；HLS 解析 m3u8 并把分片 URL 改写为
`?uuid=..&seg=N` 形式的相对引用。设备按 HLS/RFC 3986 规范相对「播放列表 URL」
解析分片地址，因此天然适配 /podcast 前缀等反向代理挂载，无需在服务端拼绝对 URL。
"""
import threading
import time
from urllib.parse import urljoin

import requests

USER_AGENT = "NomadCast/0.2 (radio proxy; +https://nomadcast.omagine.net)"
CHUNK_SIZE = 65536
_PLAYLIST_TTL = 10.0   # 秒 — 直播 m3u8 一般几秒刷新一次

_playlist_cache: dict[str, tuple[float, str, list[str]]] = {}  # uuid -> (ts, raw, seg_urls)
_playlist_lock = threading.Lock()


def is_hls(station: dict) -> bool:
    return bool(station.get("hls")) or ".m3u8" in (station.get("url") or "")


def _fetch_playlist(uuid: str, stream_url: str) -> tuple[str, list[str]]:
    """拉取并缓存 m3u8，返回 (raw_text, 分片绝对 URL 列表)。"""
    now = time.time()
    with _playlist_lock:
        entry = _playlist_cache.get(uuid)
        if entry and now - entry[0] < _PLAYLIST_TTL:
            return entry[1], entry[2]

    resp = requests.get(stream_url, headers={"User-Agent": USER_AGENT}, timeout=30)
    resp.raise_for_status()
    raw = resp.text
    segs = [urljoin(stream_url, ln.strip())
            for ln in raw.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]

    with _playlist_lock:
        _playlist_cache[uuid] = (now, raw, segs)
    return raw, segs


def hls_playlist(uuid: str, stream_url: str) -> str:
    """返回改写后的 m3u8：分片行替换为 `?uuid=..&seg=N` 相对引用。"""
    raw, _segs = _fetch_playlist(uuid, stream_url)
    out: list[str] = []
    idx = 0
    for ln in raw.splitlines():
        s = ln.strip()
        if s and not s.startswith("#"):
            out.append(f"?uuid={uuid}&seg={idx}")
            idx += 1
        else:
            out.append(ln)
    return "\n".join(out)


def passthrough(stream_url: str):
    """返回 (status, content_type, generator)，用于 MP3/AAC 字节透传。

    `Icy-MetaData: 0` 让 icecast/shoutcast 源关闭内联元数据，避免字节流里
    混入电台曲目元数据干扰解码。
    """
    headers = {"User-Agent": USER_AGENT, "Icy-MetaData": "0"}
    resp = requests.get(stream_url, headers=headers, stream=True,
                        timeout=30, allow_redirects=True)
    resp.raise_for_status()
    content_type = resp.headers.get("Content-Type", "audio/mpeg")

    def generate():
        try:
            for chunk in resp.iter_content(chunk_size=CHUNK_SIZE):
                if chunk:
                    yield chunk
        finally:
            resp.close()

    return resp.status_code, content_type, generate()


def segment(uuid: str, stream_url: str, i: int) -> tuple[bytes | None, str | None]:
    """返回第 i 个分片的 (bytes, content_type)；越界返回 (None, None)。"""
    _raw, segs = _fetch_playlist(uuid, stream_url)
    if i < 0 or i >= len(segs):
        return None, None
    resp = requests.get(segs[i], headers={"User-Agent": USER_AGENT}, timeout=30)
    resp.raise_for_status()
    return resp.content, resp.headers.get("Content-Type", "video/mp2t")
