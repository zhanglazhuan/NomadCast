"""音频下载模块

支持通用 HTTP 下载 + 小宇宙系（xyzfm）带鉴权 headers 下载。
"""
import os
import re
import requests
from pathlib import Path
from urllib.parse import urlparse


# 小宇宙 CDN 需要的请求头
XYZFM_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
    ),
    "Referer": "https://www.xiaoyuzhoufm.com/",
}


def _is_xyzfm_url(url: str) -> bool:
    """判断是否是小宇宙 CDN 链接"""
    return "xiaoyuzhoufm.com" in url or "xyzcdn" in url


def _safe_filename(episode_title: str) -> str:
    """去除文件名中的非法字符"""
    return re.sub(r'[\\/*?:"<>|]', "_", episode_title).strip()


def download_episode(
    audio_url: str,
    save_dir: str | Path,
    filename: str | None = None,
    timeout: int = 120,
) -> dict:
    """
    下载单集音频。

    Args:
        audio_url: 音频直链
        save_dir:  保存目录
        filename:  自定义文件名（不含扩展名），默认从 URL 提取
        timeout:   下载超时秒数

    Returns:
        {"success": True/False, "filepath": "...", "size": bytes, "format": "m4a/mp3"}
    """
    save_dir = Path(save_dir)
    save_dir.mkdir(parents=True, exist_ok=True)

    # 确定文件扩展名
    parsed = urlparse(audio_url)
    path_part = parsed.path
    if path_part.endswith(".m4a"):
        ext = ".m4a"
    elif path_part.endswith(".mp3"):
        ext = ".mp3"
    elif path_part.endswith(".mp4"):
        ext = ".mp4"
    else:
        ext = ".m4a"  # 默认 AAC

    if filename is None:
        filename = Path(path_part).stem
    filename = _safe_filename(filename) + ext
    filepath = save_dir / filename

    # 发起请求
    headers = XYZFM_HEADERS if _is_xyzfm_url(audio_url) else {
        "User-Agent": "Leisound/1.0"
    }

    resp = requests.get(audio_url, headers=headers, stream=True, timeout=timeout)
    resp.raise_for_status()

    total = 0
    with open(filepath, "wb") as f:
        for chunk in resp.iter_content(chunk_size=8192):
            if chunk:
                f.write(chunk)
                total += len(chunk)

    return {
        "success": True,
        "filepath": str(filepath),
        "size": total,
        "format": ext.lstrip("."),
        "content_type": resp.headers.get("Content-Type", "unknown"),
    }


def download_episodes_batch(
    episodes: list[dict],
    save_dir: str | Path,
    limit: int | None = None,
) -> list[dict]:
    """
    批量下载单集。

    Args:
        episodes: parse_feed() 返回的 episodes 列表
        save_dir: 保存目录
        limit:   最多下载几集（None = 全部）

    Returns:
        每集下载结果的列表
    """
    results = []
    for ep in episodes[:limit]:
        title = ep.get("title", "unknown")
        url = ep.get("audio_url", "")
        if not url:
            results.append({"success": False, "title": title, "error": "无音频链接"})
            continue
        try:
            r = download_episode(url, save_dir, filename=title)
            r["title"] = title
            results.append(r)
        except Exception as e:
            results.append({"success": False, "title": title, "error": str(e)})
    return results
