"""Leisound Podcast 聚合服务 — 主入口

横向对接 Apple Podcasts 搜索 & RSS 解析，向下为端侧设备提供干净 JSON。
支持小宇宙（xyzfm）音频下载。

启动方式:
    python server.py
    # 服务运行在 http://0.0.0.0:5000
"""
import os
from pathlib import Path
from flask import Flask, request, jsonify, send_file, Response
from podcast_service.search import search_podcasts, search_episodes
from podcast_service.rss_parser import parse_feed
from podcast_service.downloader import download_episode, download_episodes_batch
from podcast_service.genres import get_genres, get_genre_tree
from podcast_service.charts import get_top_podcasts, get_top_episodes
from podcast_service.lookup import lookup_podcast, batch_lookup_podcasts
from accounts import register, login
from blacklist import (is_blacklisted, add as blacklist_add,
                       remove as blacklist_remove, all_ids as blacklist_all)

app = Flask(__name__)
app.json.compact = True  # Flask 3.x compact JSON

# 下载目录
DOWNLOAD_DIR = Path(__file__).parent / "downloads"
# OTA 固件目录 — 放 NomadCast.bin 和 version.txt
FIRMWARE_DIR = Path(__file__).parent / "firmware"


@app.route("/")
def index():
    """服务状态页"""
    return jsonify({
        "service": "Leisound Podcast Aggregation",
        "version": "0.2.0",
        "endpoints": {
            # 账号
            "POST /api/register": "注册 — body: {name, password, device_id} → {user_id}",
            "POST /api/login":    "登录 — body: {name, password} → {user_id}",
            # 搜索
            "/api/search?q=<keyword>": "搜索播客（可选: &type=episode&country=cn&limit=10）",
            # 分类 & 排行榜
            "/api/genres": "获取全部播客分类列表",
            "/api/genres/tree": "获取播客分类树（嵌套层级）",
            "/api/charts/top?country=cn&limit=25": "热门播客排行榜（可选: &genre_id=1301）",
            "/api/charts/episodes?country=cn&limit=25": "热门单集排行榜",
            # 详情
            "/api/lookup?id=<collection_id>": "按 ID 获取播客详情（含 feedUrl）",
            "/api/lookup/batch?ids=1,2,3": "批量查询播客详情（最多50个）",
            # 解析
            "/api/episodes?feed_url=<url>": "解析 RSS 获取单集列表（可选: &offset=0&limit=50）",
            # 黑名单
            "/api/blacklist": "查看频道黑名单",
            # 播放 & 下载
            "/api/play?url=<audio_url>": "流式播放音频（验证用）",
            "/api/download?url=<audio_url>&title=<name>": "下载单集音频",
            "/api/download-batch?feed_url=<url>&limit=3": "批量下载 RSS 前 N 集",
        },
    })


@app.route("/api/search")
def api_search():
    """
    搜索播客 / 单集 — 代理 Apple Podcasts Search API。

    Query params:
        q       - 搜索关键词（必填）
        type    - "podcast"(默认) 搜专辑, "episode" 搜单集
        country - 国家代码（可选，默认 cn）
        limit   - 返回数量（可选，默认 10）
    """
    keyword = request.args.get("q", "").strip()
    if not keyword:
        return jsonify({"error": "缺少 q 参数"}), 400

    search_type = request.args.get("type", "podcast")
    country = request.args.get("country", "cn")
    try:
        limit = int(request.args.get("limit", 10))
    except ValueError:
        return jsonify({"error": "limit 必须是整数"}), 400

    try:
        if search_type == "episode":
            results = search_episodes(keyword, country=country, limit=limit)
        else:
            results = search_podcasts(keyword, country=country, limit=limit)
        return jsonify({"results": results, "count": len(results), "type": search_type})
    except Exception as e:
        return jsonify({"error": f"搜索失败: {e}"}), 502


@app.route("/api/episodes")
def api_episodes():
    """
    解析播客 RSS — 服务端下载 + 解析 XML，返回精简 JSON。

    Query params:
        feed_url      - RSS feed URL（必填）
        offset        - 分页偏移（可选，默认 0）
        limit         - 每页数量（可选，默认 50）
        collection_id - 频道 ID（用于黑名单记录）
    """
    feed_url = request.args.get("feed_url", "").strip()
    if not feed_url:
        return jsonify({"error": "缺少 feed_url 参数"}), 400

    cid_str = request.args.get("collection_id", "").strip()
    try:
        offset = int(request.args.get("offset", 0))
    except ValueError:
        offset = 0
    try:
        limit = int(request.args.get("limit", 50))
    except ValueError:
        limit = 50

    try:
        data = parse_feed(feed_url, offset=offset, limit=limit)
        return jsonify(data)
    except ValueError as e:
        if cid_str:
            try: blacklist_add(int(cid_str))
            except Exception: pass
        return jsonify({"error": str(e)}), 502
    except Exception as e:
        if cid_str:
            try: blacklist_add(int(cid_str))
            except Exception: pass
        return jsonify({"error": f"Parse failed: {e}"}), 502


@app.route("/api/download")
def api_download():
    """
    下载单集音频。

    Query params:
        url   - 音频直链（必填，来自 /api/episodes 的 audio_url）
        title - 文件名（可选）
    """
    audio_url = request.args.get("url", "").strip()
    if not audio_url:
        return jsonify({"error": "缺少 url 参数"}), 400

    title = request.args.get("title", "").strip() or None

    try:
        result = download_episode(audio_url, DOWNLOAD_DIR, filename=title)
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": f"下载失败: {e}", "success": False}), 502


@app.route("/api/raw")
def api_raw():
    """
    Simple byte-forwarding proxy — no transcoding, no buffering.
    Used by the ESP32 download worker to save audio files to SD card.

    Query params:
        url - audio direct link (required)
    """
    import requests as req

    audio_url = request.args.get("url", "").strip()
    if not audio_url:
        return jsonify({"error": "缺少 url 参数"}), 400

    try:
        headers = {"User-Agent": "Leisound/1.0"}
        upstream = req.get(audio_url, headers=headers, stream=True, timeout=30)
        upstream.raise_for_status()

        content_type = upstream.headers.get("Content-Type", "application/octet-stream")

        def generate():
            for chunk in upstream.iter_content(chunk_size=65536):
                if chunk:
                    yield chunk

        # Forward the upstream size so the ESP32 knows the total up front —
        # enables an accurate download % and a byte-based ETA on the device.
        # (Without it Flask streams chunked and the device sees size = unknown.)
        fwd_headers = {"Cache-Control": "no-cache"}
        clen = upstream.headers.get("Content-Length")
        if clen:
            fwd_headers["Content-Length"] = clen

        return Response(
            generate(),
            content_type=content_type,
            headers=fwd_headers,
        )
    except Exception as e:
        return jsonify({"error": f"下载失败: {e}"}), 502


@app.route("/api/play")
def api_play():
    """
    Proxy-play audio for the ESP32 ADF pipeline.

    Most podcast CDNs serve M4A with the 'moov' container index at the end of
    the file.  The ADF AAC decoder needs the index before it can parse audio
    data → naive progressive-download streaming fails for these files.

    Instead of pre-downloading the whole episode (which left the ESP32 in
    dead-air for tens of seconds and caused `Failed to get head buf data` /
    total_bytes=0 teardowns), feed the URL straight to ffmpeg.  ffmpeg issues
    HTTP range requests to seek to the trailing 'moov' atom, then streams audio
    frames progressively → the first ADTS bytes reach the ESP32 in ~1-2 s.

    Killing ffmpeg on client disconnect also prevents the download pile-up that
    happened when the device restarted the same URL.

    Query params:
        url - audio direct link (required)
    """
    import subprocess

    audio_url = request.args.get("url", "").strip()
    if not audio_url:
        return jsonify({"error": "缺少 url 参数"}), 400

    # ffmpeg reads the remote file directly (seekable via HTTP range) and
    # re-encodes to ADTS AAC (streamable: every frame carries its own header).
    proc = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-user_agent", "Leisound/1.0",
         "-reconnect", "1", "-reconnect_streamed", "1",
         "-reconnect_delay_max", "5",
         "-i", audio_url,
         "-c:a", "aac", "-b:a", "96k",
         "-f", "adts", "pipe:1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def generate():
        try:
            while True:
                data = proc.stdout.read(65536)
                if not data:
                    break
                yield data
        finally:
            # Client gone or stream ended → don't leave ffmpeg downloading.
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    return Response(
        generate(),
        content_type="audio/aac",
        headers={"Cache-Control": "no-cache"},
    )


@app.route("/api/download-batch")
def api_download_batch():
    """
    解析 RSS 并批量下载前 N 集。

    Query params:
        feed_url - RSS feed URL（必填）
        limit    - 下载数量（可选，默认 3）
    """
    feed_url = request.args.get("feed_url", "").strip()
    if not feed_url:
        return jsonify({"error": "缺少 feed_url 参数"}), 400

    try:
        limit = int(request.args.get("limit", 3))
    except ValueError:
        return jsonify({"error": "limit 必须是整数"}), 400

    try:
        podcast = parse_feed(feed_url)
        results = download_episodes_batch(
            podcast["episodes"], DOWNLOAD_DIR, limit=limit
        )
        return jsonify({
            "podcast": podcast["title"],
            "downloaded": [r for r in results if r.get("success")],
            "failed": [r for r in results if not r.get("success")],
            "total": len(results),
        })
    except Exception as e:
        return jsonify({"error": f"批量下载失败: {e}", "success": False}), 502


# ─── 分类 & 排行榜 ────────────────────────────────────────


@app.route("/api/genres")
def api_genres():
    """
    获取播客全部分类（扁平列表）。

    Query params:
        format  - "flat" 扁平列表（默认）, "tree" 嵌套树
    """
    fmt = request.args.get("format", "flat")
    try:
        if fmt == "tree":
            data = get_genre_tree()
        else:
            data = get_genres()
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": f"获取分类失败: {e}"}), 502


@app.route("/api/genres/tree")
def api_genres_tree():
    """获取播客分类树（嵌套层级结构）"""
    try:
        data = get_genre_tree()
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": f"获取分类树失败: {e}"}), 502


@app.route("/api/charts/top")
def api_charts_top():
    """
    热门播客排行榜。

    Query params:
        country  - 国家代码（可选，默认 cn）
        limit    - 返回数量（可选，默认 25）
        genre_id - 分类 ID（可选）
    """
    country = request.args.get("country", "cn")
    try:
        limit = int(request.args.get("limit", 25))
    except ValueError:
        return jsonify({"error": "limit 必须是整数"}), 400

    genre_id = request.args.get("genre_id")
    if genre_id:
        try:
            genre_id = int(genre_id)
        except ValueError:
            return jsonify({"error": "genre_id 必须是整数"}), 400

    try:
        data = get_top_podcasts(country=country, limit=limit, genre_id=genre_id)
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": f"获取排行榜失败: {e}"}), 502


@app.route("/api/charts/episodes")
def api_charts_episodes():
    """
    热门单集排行榜。

    Query params:
        country  - 国家代码（可选，默认 cn）
        limit    - 返回数量（可选，默认 25）
        genre_id - 分类 ID（可选）
    """
    country = request.args.get("country", "cn")
    try:
        limit = int(request.args.get("limit", 25))
    except ValueError:
        return jsonify({"error": "limit 必须是整数"}), 400

    genre_id = request.args.get("genre_id")
    if genre_id:
        try:
            genre_id = int(genre_id)
        except ValueError:
            return jsonify({"error": "genre_id 必须是整数"}), 400

    try:
        data = get_top_episodes(country=country, limit=limit, genre_id=genre_id)
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": f"获取单集排行失败: {e}"}), 502


# ─── 完整排行榜（chart + lookup 合并） ────────────────────


@app.route("/api/charts/full")
def api_charts_full():
    """
    热门播客排行榜 — 一步返回完整信息（含 feedUrl、trackCount）。

    内部: chart API → batch lookup → 合并 → 返回 Apple RSS 兼容格式。

    Query params:
        country  - 国家代码（可选，默认 cn）
        limit    - 返回数量（可选，默认 50，max 50）
    """
    country = request.args.get("country", "cn")
    try:
        limit = int(request.args.get("limit", 50))
    except ValueError:
        return jsonify({"error": "limit 必须是整数"}), 400
    if limit > 50:
        limit = 50

    try:
        chart = get_top_podcasts(country=country, limit=limit)
        ids = [int(r["id"]) for r in chart["results"] if r.get("id")]

        if ids:
            details = batch_lookup_podcasts(ids, country=country)
            detail_map = {d["collection_id"]: d for d in details}
        else:
            detail_map = {}

        results = []
        for r in chart["results"]:
            cid = int(r["id"])
            if is_blacklisted(cid):
                continue  # skip previously blacklisted channels
            d = detail_map.get(cid, {})
            results.append({
                "id": cid,
                "name": r.get("name", ""),
                "artistName": r.get("artist_name", ""),
                "artworkUrl100": r.get("artwork_url", ""),
                "url": r.get("apple_url", ""),
                "feedUrl": d.get("feed_url", ""),
                "trackCount": d.get("track_count", 0),
                "primaryGenreName": r["genres"][0]["name"] if r.get("genres") else "",
                "releaseDate": d.get("release_date", ""),
            })

        return jsonify({
            "feed": {
                "title": chart.get("title", ""),
                "country": chart.get("country", country),
                "updated": chart.get("updated", ""),
                "results": results,
            }
        })
    except Exception as e:
        return jsonify({"error": f"获取排行榜失败: {e}"}), 502


# ─── 详情查询 ──────────────────────────────────────────────


@app.route("/api/lookup")
def api_lookup():
    """
    按播客 ID 获取完整信息（含 feedUrl）。

    Query params:
        id      - 播客 collectionId（必填）
        country - 国家代码（可选，默认 cn）
    """
    cid = request.args.get("id", "").strip()
    if not cid:
        return jsonify({"error": "缺少 id 参数"}), 400

    try:
        cid = int(cid)
    except ValueError:
        return jsonify({"error": "id 必须是整数"}), 400

    country = request.args.get("country", "cn")
    try:
        result = lookup_podcast(cid, country=country)
        if result is None:
            return jsonify({"error": "未找到该播客"}), 404
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": f"查询失败: {e}"}), 502


@app.route("/api/lookup/batch")
def api_lookup_batch():
    """
    批量查询播客详情（最多 50 个）。

    Query params:
        ids     - 播客 ID 列表，逗号分隔（必填，如 ids=1582119137,1573189055）
        country - 国家代码（可选，默认 cn）
    """
    ids_str = request.args.get("ids", "").strip()
    if not ids_str:
        return jsonify({"error": "缺少 ids 参数"}), 400

    try:
        ids = [int(x.strip()) for x in ids_str.split(",") if x.strip()]
    except ValueError:
        return jsonify({"error": "ids 必须为逗号分隔的整数"}), 400

    if len(ids) > 50:
        return jsonify({"error": "最多支持 50 个 ID"}), 400

    country = request.args.get("country", "cn")
    try:
        results = batch_lookup_podcasts(ids, country=country)
        return jsonify({"results": results, "total": len(results)})
    except Exception as e:
        return jsonify({"error": f"批量查询失败: {e}"}), 502


# ─── 账号系统 ──────────────────────────────────────────────

@app.route("/api/register", methods=["POST"])
def api_register():
    """
    注册新用户 — 返回唯一 user_id。

    JSON body:
        name      - 用户名
        password  - 密码
        device_id - 设备 ID (MAC 地址)
    """
    data = request.get_json(silent=True) or {}
    name = data.get("name", "").strip()
    password = data.get("password", "").strip()
    device_id = data.get("device_id", "").strip()

    if not name or not password:
        return jsonify({"error": "name 和 password 为必填"}), 400
    if not device_id:
        return jsonify({"error": "device_id 为必填"}), 400

    result = register(name, password, device_id)
    if "error" in result:
        return jsonify(result), result.get("code", 400)
    return jsonify(result), 201


@app.route("/api/login", methods=["POST"])
def api_login():
    """
    登录 — 验证凭据，返回 user_id。

    JSON body:
        name     - 用户名
        password - 密码
    """
    data = request.get_json(silent=True) or {}
    name = data.get("name", "").strip()
    password = data.get("password", "").strip()

    if not name or not password:
        return jsonify({"error": "name 和 password 为必填"}), 400

    result = login(name, password)
    if "error" in result:
        return jsonify(result), result.get("code", 400)
    return jsonify(result)


# ─── 黑名单管理 ────────────────────────────────────────────

@app.route("/api/blacklist")
def api_blacklist():
    """查看 / 管理频道黑名单。DELETE ?id=... 移出黑名单。"""
    if request.method == "DELETE":
        cid = request.args.get("id", "").strip()
        if cid:
            blacklist_remove(int(cid))
        return jsonify({"blacklist": blacklist_all()})
    return jsonify({"blacklist": blacklist_all()})


# ─── OTA 固件更新 ──────────────────────────────────────────


def _get_latest_firmware():
    """返回 (version, path, size) 或 (None, None, 0)"""
    ver_file = FIRMWARE_DIR / "version.txt"
    version = None
    if ver_file.exists():
        version = ver_file.read_text().strip()

    # 找最新的 .bin 文件
    bins = sorted(FIRMWARE_DIR.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not bins:
        return version, None, 0

    fw = bins[0]

    # 如果没有 version.txt，从文件名推断: NomadCast_v1.0.1.bin → v1.0.1
    if not version:
        import re
        m = re.search(r'v?(\d+\.\d+\.\d+)', fw.name)
        if m:
            version = m.group(1)

    return version, fw, fw.stat().st_size


@app.route("/api/ota/check")
def api_ota_check():
    """
    检查固件更新 — 返回 manifest.json 格式。

    端侧从 version.txt 或 .bin 文件名读取版本号，
    与运行中的固件版本比较。
    """
    try:
        version, fw, size = _get_latest_firmware()
        if not version or not fw:
            return jsonify({"error": "No firmware available"}), 404

        return jsonify({
            "version": version,
            "url": f"http://{request.host}/api/ota/download",
            "size": size,
            "changelog": "",
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/api/ota/download")
def api_ota_download():
    """下载固件 .bin 文件"""
    try:
        _, fw, _ = _get_latest_firmware()
        if not fw or not fw.exists():
            return jsonify({"error": "No firmware binary"}), 404

        return send_file(
            str(fw),
            mimetype="application/octet-stream",
            as_attachment=True,
            download_name=fw.name,
        )
    except Exception as e:
        return jsonify({"error": str(e)}), 500


if __name__ == "__main__":
    print("=" * 50)
    print("  Leisound Podcast 聚合服务 v0.2.0")
    print(f"  下载目录: {DOWNLOAD_DIR}")
    print(f"  固件目录: {FIRMWARE_DIR}")
    print("  地址: http://127.0.0.1:5000")
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, debug=True)
