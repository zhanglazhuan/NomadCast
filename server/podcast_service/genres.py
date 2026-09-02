"""Apple Podcasts 分类体系

从 iTunes Store Services 获取播客所有 Genre 及子分类。
每个分类自带对应的 RSS top 榜单 URL，可直接用于获取分类排行榜。
"""
import requests

GENRES_URL = "https://itunes.apple.com/WebObjects/MZStoreServices.woa/ws/genres"
GENRE_ROOT_ID = 26  # Podcasts 的顶级 genre ID


def _flatten_genres(subgenres: dict, parent_name: str = "") -> list[dict]:
    """递归展平 genre 树为列表，每项含 id, name, parent, rss_url"""
    results = []
    for gid, info in sorted(subgenres.items()):
        name = info.get("name", "")
        rss_urls = info.get("rssUrls", {})
        results.append({
            "genre_id": int(gid),
            "name": name,
            "parent": parent_name,
            "top_podcasts_rss": rss_urls.get("topPodcasts", ""),
            "top_episodes_rss": rss_urls.get("topPodcastEpisodes", ""),
        })
        children = info.get("subgenres", {})
        if children:
            results.extend(_flatten_genres(children, parent_name=name))
    return results


def get_genres(timeout: int = 10) -> dict:
    """
    获取播客完整分类体系。

    Args:
        timeout: 请求超时秒数

    Returns:
        {
            "genres": [
                {
                    "genre_id": 1301,
                    "name": "Arts",
                    "parent": "",
                    "top_podcasts_rss": "https://itunes.apple.com/us/rss/toppodcasts/genre=1301/json",
                    "top_episodes_rss": "https://itunes.apple.com/us/rss/toppodcastepisodes/genre=1301/json",
                },
                ...
            ],
            "total": 50
        }
    """
    resp = requests.get(GENRES_URL, params={"id": GENRE_ROOT_ID}, timeout=timeout)
    resp.raise_for_status()
    data = resp.json()

    root = data.get(str(GENRE_ROOT_ID), {})
    all_genres = _flatten_genres(root.get("subgenres", {}))

    return {
        "genres": all_genres,
        "total": len(all_genres),
    }


def get_genre_tree(timeout: int = 10) -> dict:
    """
    获取播客分类树（嵌套结构，保留层级关系）。

    Returns:
        {
            "genre_id": 26,
            "name": "Podcasts",
            "subgenres": [
                {
                    "genre_id": 1301,
                    "name": "Arts",
                    "subgenres": [
                        {"genre_id": 1482, "name": "Books", "subgenres": []},
                        ...
                    ]
                },
                ...
            ]
        }
    """
    resp = requests.get(GENRES_URL, params={"id": GENRE_ROOT_ID}, timeout=timeout)
    resp.raise_for_status()
    data = resp.json()

    def build_tree(node: dict) -> dict:
        return {
            "genre_id": int(node.get("id", 0)),
            "name": node.get("name", ""),
            "top_podcasts_rss": node.get("rssUrls", {}).get("topPodcasts", ""),
            "subgenres": [build_tree(child) for child in node.get("subgenres", {}).values()],
        }

    root = data.get(str(GENRE_ROOT_ID), {})
    return build_tree(root)
