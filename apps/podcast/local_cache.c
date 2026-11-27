/**
 * @file local_cache.c
 * @brief Local download library index — one JSON file per channel (bucketed).
 *
 * Layout: /sdcard/.nomadcast/downloads/.meta/<collection_id>.json
 *   { "cid":<canon>, "col":<collection_id>, "ch":"<title>",
 *     "episodes":[ {"eid","ep","url","dur"}, ... ] }
 *
 * Why buckets: a download completion rewrites only ONE small channel file
 * (O(episodes-in-channel)), not the whole library. The stable channel identity
 * is `collection_id` (falls back to the transient chart id `cid`), so the same
 * podcast always maps to the same file — channel dedup is inherent, and episode
 * dedup is by the stable `audio_url`. Each file is written directly + fsync:
 * FatFs flushes a file's own directory entry on f_sync but only writes back a
 * rename's directory update lazily, so the old tmp+rename left buckets invisible
 * after a power-cut/reboot.
 */
#include "local_cache.h"
#include "model.h"
#include "app.h"
#include "mp4_remux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#elif defined(ESP_PLATFORM)
#include <unistd.h>
#include "esp_log.h"
#define mkdir_p(p) mkdir(p, 0755)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

#define DL_META_DIR   "/sdcard/.nomadcast/downloads/.meta"        /* per-channel buckets */
#define DL_META_OLD   "/sdcard/.nomadcast/downloads/.meta.json"   /* legacy monolith (migration) */
#define DL_LOG_PATH   "/sdcard/.nomadcast/downloads/dl.log"
#define DL_BASE_PATH  "/sdcard/.nomadcast/downloads"              /* downloaded audio tree */

/* ── JSON helpers ──────────────────────────────────────────────────────── */

static bool json_get_str(const char *json, const char *key, char *out, int max) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < max - 1) {
        if (*p == '\\') p++;
        if (*p) out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static int json_get_int(const char *json, const char *key, int def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    return atoi(p);
}

static long long json_get_ll(const char *json, const char *key, long long def) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    return atoll(p);
}

static void json_write_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void downloads_ensure_dir(void) {
    char tmp[256] = "/sdcard/.nomadcast";
    mkdir_p(tmp);
    strcat(tmp, "/downloads");
    mkdir_p(tmp);
    strcat(tmp, "/.meta");     /* → DL_META_DIR */
    mkdir_p(tmp);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f); fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* ── In-RAM model update (shared by add / load / migrate) ─────────────────── */

static void local_audio_path(const char *ch_title, const char *ep_title,
                             char *out, int out_sz);

static const char *local_channel_title(struct PodcastApp *app, int chan_id) {
    PodcastModel *m = app->model;
    for (int j = 0; j < m->local_channel_count; j++)
        if (m->local_channels[j].id == chan_id) return m->local_channels[j].title;
    return NULL;
}

/* Add one episode to the model, keyed by canonical channel id. Dedups the
 * episode by stable audio_url, by (channel, title), and by the on-SD file path.
 * Returns true if a NEW episode was inserted (so the caller knows to persist
 * that channel's bucket). */
static bool local_model_add(struct PodcastApp *app, int canon, int col,
                            const char *ch_title, int eid, const char *ep_title,
                            const char *url, int dur, time_t dl_time,
                            const char *pub_date) {
    PodcastModel *m = app->model;

    if (url && url[0])
        for (int i = 0; i < m->local_episode_count; i++)
            if (strcmp(m->local_episodes[i].audio_url, url) == 0) return false;  /* dup */

    /* Also dedup by (channel, title). A recovered orphan is keyed by its local
     * file path while a normal download is keyed by the CDN url — the same
     * episode with two different audio_url values — so the url check alone
     * would let a later cache_local_add insert a duplicate of an episode the
     * recovery already indexed. */
    if (ep_title && ep_title[0])
        for (int i = 0; i < m->local_episode_count; i++)
            if (m->local_episodes[i].channel_id == canon &&
                strcmp(m->local_episodes[i].title, ep_title) == 0)
                return false;   /* dup */

    /* Dedup by the derived on-SD path. The feed title may contain characters
     * (:/? etc.) that get sanitized to '_' in the filename, so a recovered
     * entry (title from the sanitized filename) compares unequal to a normal
     * entry (raw feed title) above — yet both resolve to the SAME file. The
     * path is the one identity that always matches, and it is also what makes
     * two entries "the same episode" from the user's point of view. */
    {
        char cand_path[1536];
        local_audio_path(ch_title, ep_title, cand_path, sizeof(cand_path));
        for (int i = 0; i < m->local_episode_count; i++) {
            const char *ct = local_channel_title(app, m->local_episodes[i].channel_id);
            if (!ct) continue;
            char ex_path[1536];
            local_audio_path(ct, m->local_episodes[i].title, ex_path, sizeof(ex_path));
            if (strcmp(cand_path, ex_path) == 0) return false;   /* same file */
        }
    }

    int en = m->local_episode_count + 1;
    Episode *ne = (Episode *)realloc(m->local_episodes, en * sizeof(Episode));
    if (!ne) return false;
    m->local_episodes = ne;
    Episode *e = &m->local_episodes[m->local_episode_count];
    memset(e, 0, sizeof(Episode));
    e->id = eid; e->channel_id = canon; e->duration_sec = dur;
    e->downloaded_at = dl_time;
    snprintf(e->title, sizeof(e->title), "%s", ep_title ? ep_title : "");
    snprintf(e->audio_url, sizeof(e->audio_url), "%s", url ? url : "");
    snprintf(e->pub_date, sizeof(e->pub_date), "%s", pub_date ? pub_date : "");
    m->local_episode_count = en;

    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == canon) { m->local_channels[i].episode_count++; return true; }

    int cn = m->local_channel_count + 1;
    Channel *nc = (Channel *)realloc(m->local_channels, cn * sizeof(Channel));
    if (nc) {
        m->local_channels = nc;
        Channel *c = &m->local_channels[m->local_channel_count];
        memset(c, 0, sizeof(Channel));
        c->id = canon; c->downloaded = true; c->episode_count = 1;
        c->collection_id = col;
        snprintf(c->title, sizeof(c->title), "%s", ch_title ? ch_title : "");
        c->category = CHANNEL_CATEGORY_NEWS_SOCIETY;
        c->card_color = 0x4CAF50 + (m->local_channel_count * 0x12345) % 0x1000000;
        m->local_channel_count = cn;
    }
    return true;
}

/* Next monotonic download sequence number. Wall-clock time(NULL) is unreliable
 * before SNTP syncs (it returns 0), so using it as `downloaded_at` made every
 * download share the same timestamp and collapsed 最新下载/最早下载 into the id
 * tie-break. Each new download is instead assigned one past the largest existing
 * value — a strict, durable ordering: the max is reloaded from the bucket JSON
 * on boot, so it never goes backwards across reboots. */
static time_t next_download_seq(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    time_t max = 0;
    for (int i = 0; i < m->local_episode_count; i++)
        if (m->local_episodes[i].downloaded_at > max)
            max = m->local_episodes[i].downloaded_at;
    return max + 1;
}

/* Does any indexed episode resolve to `path` on SD?  Mirrors local_model_add's
 * path dedup so recovery can skip already-indexed files without re-probing them
 * (a full-file probe on every app open is what made the Podcast app take minutes
 * to launch). */
static bool local_model_has_path(struct PodcastApp *app, const char *path) {
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++) {
        const char *ct = local_channel_title(app, m->local_episodes[i].channel_id);
        if (!ct) continue;
        char ex[1536];
        local_audio_path(ct, m->local_episodes[i].title, ex, sizeof(ex));
        if (strcmp(ex, path) == 0) return true;
    }
    return false;
}

/* ── Bucket file I/O ──────────────────────────────────────────────────────── */

static void channel_file_path(char *out, int sz, int canon) {
    snprintf(out, sz, "%s/%d.json", DL_META_DIR, canon);
}

/* Rewrite one channel's bucket from the current in-RAM model.  Written
 * directly to the final path (no tmp+rename): FatFs flushes a file's own
 * directory entry on f_sync, but the directory update from f_rename is only
 * written back lazily, so the old swap left the bucket invisible after a
 * power-cut/reboot — downloaded audio then vanished from the local list. */
static void local_write_channel(struct PodcastApp *app, int canon) {
    PodcastModel *m = app->model;
    const Channel *c = NULL;
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == canon) { c = &m->local_channels[i]; break; }
    if (!c) return;

    char path[320];
    channel_file_path(path, sizeof(path), canon);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"cid\":%d,\"col\":%d,\"ch\":", canon, c->collection_id);
    json_write_str(f, c->title);
    fprintf(f, ",\"episodes\":[\n");
    bool first = true;
    for (int i = 0; i < m->local_episode_count; i++) {
        Episode *e = &m->local_episodes[i];
        if (e->channel_id != canon) continue;
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f, "{\"eid\":%d,\"ep\":", e->id); json_write_str(f, e->title);
        fprintf(f, ",\"url\":"); json_write_str(f, e->audio_url);
        fprintf(f, ",\"dur\":%d,\"dl\":%lld", e->duration_sec,
                (long long)e->downloaded_at);
        fprintf(f, ",\"pub\":"); json_write_str(f, e->pub_date);
        fprintf(f, "}");
    }
    fprintf(f, "\n]}\n");
    fflush(f);
#if defined(ESP_PLATFORM)
    fsync(fileno(f));      /* force write-through — makes the bucket durable */
#endif
    fclose(f);
}

/* Parse one bucket file's contents into the model. */
static void parse_channel_buf(struct PodcastApp *app, const char *buf) {
    int canon = json_get_int(buf, "cid", 0);
    int col   = json_get_int(buf, "col", 0);
    char ch_title[256];
    json_get_str(buf, "ch", ch_title, sizeof(ch_title));
    if (canon <= 0) return;

    const char *ep = buf;
    for (;;) {
        const char *obj = strstr(ep, "{\"eid\":");
        if (!obj) break;
        ep = obj + 1;
        int eid = json_get_int(obj, "eid", 0);
        int dur = json_get_int(obj, "dur", 0);
        time_t dl = (time_t)json_get_ll(obj, "dl", 0);
        static char eptitle[256], url[1024], pub[128];   /* boot-time, single-threaded */
        json_get_str(obj, "ep",  eptitle, sizeof(eptitle));
        json_get_str(obj, "url", url, sizeof(url));
        json_get_str(obj, "pub", pub, sizeof(pub));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur, dl, pub);
    }
}

static void load_buckets(struct PodcastApp *app) {
    DIR *d = opendir(DL_META_DIR);
    if (!d) return;
    struct dirent *de;
    char path[320];
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;                 /* skip . .. and .tmp-hidden */
        size_t len = strlen(de->d_name);
        if (len < 6 || strcmp(de->d_name + len - 5, ".json") != 0) continue;  /* only *.json */
        snprintf(path, sizeof(path), "%s/%s", DL_META_DIR, de->d_name);
        char *buf = read_file(path);
        if (!buf) continue;
        parse_channel_buf(app, buf);
        free(buf);
    }
    closedir(d);
}

/* FNV-1a 32-bit hash → positive int (used to derive stable, collision-rare ids
 * for channels/episodes recovered from filenames alone). */
static int fnv1a_positive(const char *s) {
    uint32_t h = 2166136261u;
    for (const char *p = s; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return (int)((h & 0x7FFFFFFFu) | 1u);   /* keep > 0 so bucket/parse accepts it */
}

/* Rebuild the on-SD path a downloaded episode lives at, from channel + episode
 * titles.  Mirrors controller.c's podcast_local_audio_path() so recovery/prune
 * can never diverge from the downloader/player naming. */
static void local_audio_path(const char *ch_title, const char *ep_title,
                             char *out, int out_sz)
{
    char ch_safe[256], ep_safe[512];
    snprintf(ch_safe, sizeof(ch_safe), "%s", ch_title ? ch_title : "unknown");
    snprintf(ep_safe, sizeof(ep_safe), "%s.aac", ep_title ? ep_title : "audio");
    for (char *p = ch_safe; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    for (char *p = ep_safe; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    snprintf(out, out_sz, DL_BASE_PATH "/%s/%s", ch_safe, ep_safe);
}

static bool read_u32_at(FILE *f, long off, uint32_t *out) {
    unsigned char b[4];
    if (fseek(f, off, SEEK_SET) != 0) return false;
    if (fread(b, 1, 4, f) != 4) return false;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return true;
}

static bool read_u64_at(FILE *f, long off, uint64_t *out) {
    unsigned char b[8];
    if (fseek(f, off, SEEK_SET) != 0) return false;
    if (fread(b, 1, 8, f) != 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    *out = v;
    return true;
}

/* Probe a local .m4a: is it a COMPLETE, playable file, and (optionally) how
 * long is it?  "Complete" means the top-level boxes tile cleanly to EOF — a
 * truncated download has its trailing (mdat) box cut short, so the walk stops
 * before EOF.  Merely finding a moov atom is NOT enough: some encoders put
 * moov at the END (a truncated file then lacks moov), but "fast-start" files
 * put moov at the FRONT, so a partial download of those still has a valid moov
 * and would otherwise look complete.  Walking the box headers is a handful of
 * 8-byte reads + seeks, O(top-level boxes), not O(file size).  duration_sec,
 * when non-NULL, is filled from moov→mvhd (timescale/duration); left 0 if
 * unknown. */
static bool mp4_probe(const char *path, int *duration_sec) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long file_size = ftell(f);
    if (file_size < 8) { fclose(f); return false; }
    if (duration_sec) *duration_sec = 0;

    /* Pass 1 — walk the top-level boxes. Find moov (for the duration below)
     * and verify the file is COMPLETE: every box must tile cleanly to EOF.
     * A partial download has its last (mdat) box truncated, so the walk stops
     * short of EOF. Merely "has a moov atom" is not enough — fast-start M4A
     * files put moov at the front, so a partial file still contains moov. */
    long moov_payload = -1, moov_len = 0;
    bool complete = false;
    long off = 0;
    while (off >= 0 && file_size - off >= 8) {
        if (fseek(f, off, SEEK_SET) != 0) break;
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;

        unsigned long size = ((unsigned long)hdr[0] << 24) |
                             ((unsigned long)hdr[1] << 16) |
                             ((unsigned long)hdr[2] << 8) |
                             ((unsigned long)hdr[3]);
        char type[5] = { hdr[4], hdr[5], hdr[6], hdr[7], 0 };
        unsigned long hdr_sz = 8;
        if (size == 1) {                       /* 64-bit extended size */
            unsigned char ext[8];
            if (fread(ext, 1, 8, f) != 8) break;
            size = 0;
            for (int i = 0; i < 8; i++) size = (size << 8) | ext[i];
            hdr_sz = 16;
        } else if (size == 0) {                /* box runs to EOF */
            size = (unsigned long)(file_size - off);
        }
        if (size < hdr_sz) break;              /* corrupt box header */
        if (size > (unsigned long)(file_size - off)) break;   /* past EOF → truncated */

        if (strcmp(type, "moov") == 0) {
            moov_payload = off + (long)hdr_sz;
            moov_len = (long)(size - hdr_sz);
        }
        off += (long)size;
        if (off == file_size) { complete = true; break; }
    }
    bool has_moov = (moov_payload >= 0) && complete;

    /* Pass 2 — inside moov, find mvhd and read the duration. */
    if (has_moov && duration_sec && moov_len >= 8) {
        long moov_end = moov_payload + moov_len;
        if (moov_end > file_size) moov_end = file_size;
        long p = moov_payload;
        while (p >= 0 && moov_end - p >= 8) {
            if (fseek(f, p, SEEK_SET) != 0) break;
            unsigned char hdr[8];
            if (fread(hdr, 1, 8, f) != 8) break;
            unsigned long size = ((unsigned long)hdr[0] << 24) |
                                 ((unsigned long)hdr[1] << 16) |
                                 ((unsigned long)hdr[2] << 8) |
                                 ((unsigned long)hdr[3]);
            char type[5] = { hdr[4], hdr[5], hdr[6], hdr[7], 0 };
            unsigned long hdr_sz = 8;
            if (size == 1) {
                unsigned char ext[8];
                if (fread(ext, 1, 8, f) != 8) break;
                size = 0;
                for (int i = 0; i < 8; i++) size = (size << 8) | ext[i];
                hdr_sz = 16;
            } else if (size == 0) {
                size = (unsigned long)(moov_end - p);
            }
            if (size < hdr_sz) break;
            if (size > (unsigned long)(moov_end - p)) break;

            if (strcmp(type, "mvhd") == 0) {
                unsigned char ver;
                if (fseek(f, p + 8, SEEK_SET) == 0 && fread(&ver, 1, 1, f) == 1) {
                    uint32_t timescale = 0;
                    uint64_t duration = 0;
                    if (ver == 1) {
                        read_u32_at(f, p + 28, &timescale);
                        read_u64_at(f, p + 32, &duration);
                    } else {
                        uint32_t dur32 = 0;
                        read_u32_at(f, p + 20, &timescale);
                        read_u32_at(f, p + 24, &dur32);
                        duration = dur32;
                    }
                    if (timescale > 0) *duration_sec = (int)(duration / timescale);
                }
                break;
            }
            p += (long)size;
        }
    }

    fclose(f);
    return has_moov;
}

/* Public complete-file check — lets the controller mark a network episode as
 * "downloaded" (green check) without exposing the box parser. Handles both the
 * MP4/M4A container (moov sample table) and the remuxed AAC-ADTS stream. */
static bool aac_head_is_adts(const char *path);   /* defined below */

bool cache_local_file_complete(const char *path) {
    if (mp4_probe(path, NULL)) return true;
    /* ADTS: existence + a valid frame head + non-zero size is enough. The .aac
     * is only written after the download's remux completes (tmp+rename), so a
     * present ADTS file is complete — no full-frame count, which froze the
     * network channel page rendering the green check for a downloaded episode. */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0 && aac_head_is_adts(path)) return true;
    return false;
}

int cache_local_file_duration(const char *path) {
    int dur = 0;
    if (mp4_probe(path, &dur)) return dur;
    if (aac_file_probe(path, &dur)) return dur;
    return 0;
}

int cache_local_duration_for_path(struct PodcastApp *app, const char *path) {
    if (!app || !app->model || !path) return 0;
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++) {
        const char *ct = local_channel_title(app, m->local_episodes[i].channel_id);
        if (!ct) continue;
        char ex[1536];
        local_audio_path(ct, m->local_episodes[i].title, ex, sizeof(ex));
        if (strcmp(ex, path) == 0) return m->local_episodes[i].duration_sec;
    }
    return 0;
}

/* Cheap ADTS sniff — reads only the first 7-byte frame header, not the whole
 * file.  Distinguishes a genuine ADTS stream from an MP4-in-.aac leftover (whose
 * first bytes are an 'ftyp'/'moov' box) without a full-file probe. */
static bool aac_head_is_adts(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char h[7];
    if (fread(h, 1, 7, f) != 7) { fclose(f); return false; }
    fclose(f);
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return false;
    uint32_t fl = ((uint32_t)(h[3] & 3) << 11) | ((uint32_t)h[4] << 3) |
                  ((uint32_t)(h[5] >> 5) & 7);
    return fl >= 7 && fl <= 8191;
}

/* Recover downloaded audio whose .meta bucket was lost (power cut before the
 * directory flush, or an SD card migrated without its index), or whose download
 * failed after the file was fully written (the task never registered the
 * episode). Scans the downloads tree and re-indexes any complete .aac file
 * missing from the local model — including individual episodes of a channel
 * that already exists. Legacy .m4a files (from before the ADTS remux) are
 * remuxed to .aac on the way. Metadata that can't be recovered from a filename
 * (CDN url, duration, collection id) is left empty/zero; playback still works
 * because the local path is re-derived from channel + episode titles. */
static void recover_orphaned_downloads(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    if (!m) return;

    DIR *root = opendir(DL_BASE_PATH);
    if (!root) return;

    struct dirent *de;
    while ((de = readdir(root)) != NULL) {
        if (de->d_name[0] == '.') continue;   /* skip . .. .meta */

        char chdir[384];
        snprintf(chdir, sizeof(chdir), "%s/%s", DL_BASE_PATH, de->d_name);
        struct stat st;
        if (stat(chdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        const char *ch_title = de->d_name;

        /* Recover into the existing channel if one is already registered (a
         * prior download registered the channel but a later failed download left
         * an orphaned file for another episode). Reuse its canonical id so the
         * recovered episodes attach to it instead of forking a duplicate
         * channel; missing channels still fall back to the title hash. */
        int canon = 0, col = 0;
        bool found = false;
        for (int i = 0; i < m->local_channel_count; i++) {
            if (strcmp(m->local_channels[i].title, ch_title) == 0) {
                canon = m->local_channels[i].id;
                col   = m->local_channels[i].collection_id;
                found = true;
                break;
            }
        }
        if (!found) canon = fnv1a_positive(ch_title);
        printf("[DBG] recover: dir='%s' found=%d canon=%d\n", ch_title, found, canon);
        fflush(stdout);

        DIR *cd = opendir(chdir);
        if (!cd) continue;

        bool added_any = false;
        struct dirent *fde;
        while ((fde = readdir(cd)) != NULL) {
            if (fde->d_name[0] == '.') continue;
            size_t len = strlen(fde->d_name);
            bool is_aac = (len >= 5 && strcmp(fde->d_name + len - 4, ".aac") == 0);
            bool is_m4a = (len >= 5 && strcmp(fde->d_name + len - 4, ".m4a") == 0);
            if (!is_aac && !is_m4a) continue;

            char ep_title[256];
            snprintf(ep_title, sizeof(ep_title), "%.*s", (int)(len - 4), fde->d_name);

            char aac_path[640];
            snprintf(aac_path, sizeof(aac_path), "%s/%.*s.aac",
                     chdir, (int)(len - 4), fde->d_name);

            /* Fast path: an already-indexed episode needs no probe/remux — its
             * duration is persisted in the channel bucket.  Re-probing would
             * re-read the whole file on every app open (the "podcast app takes
             * minutes to launch" bug). */
            if (local_model_has_path(app, aac_path)) continue;

            if (is_m4a) {
                /* Legacy pre-remux file: convert MP4→ADTS so resume can seek.
                 * Only a COMPLETE MP4 is migrated — a mid-download partial .m4a
                 * (truncated mdat) is left for the download worker to resume. */
                char fpath[640];
                snprintf(fpath, sizeof(fpath), "%s/%s", chdir, fde->d_name);
                int d = 0;
                if (!mp4_probe(fpath, &d)) continue;   /* partial/incomplete — skip */
                if (!mp4_remux_to_adts(fpath, aac_path, &d))
                    mp4_copy_file(fpath, aac_path);
                remove(fpath);          /* drop the source m4a once migrated */
            } else {
                /* An existing .aac that fails the ADTS probe but IS a complete
                 * MP4 is a leftover from the old copy-fallback (or a naive
                 * .m4a→.aac rename): the bytes are MP4, so byte-seek resume can
                 * never work and the player would stall.  Re-remux it in place
                 * so it becomes a genuinely seekable ADTS stream.  On failure,
                 * leave it — the player now sniffs content and plays it from
                 * the start instead of stalling. */
                int d = 0;
                if (!aac_head_is_adts(aac_path) && mp4_probe(aac_path, &d)) {
                    char tmp[672];
                    snprintf(tmp, sizeof(tmp), "%s.tmp", aac_path);
                    printf("[DBG] recover: re-remux MP4-in-.aac %s\n", aac_path);
                    fflush(stdout);
                    if (mp4_remux_to_adts(aac_path, tmp, &d)) {
                        remove(aac_path);
                        if (rename(tmp, aac_path) != 0) remove(tmp);
                    } else {
                        remove(tmp);
                    }
                }
            }

            /* Index only complete ADTS files — skip empty/truncated ones. */
            int dur = 0;
            if (!aac_file_probe(aac_path, &dur)) continue;

            /* local_model_add dedups by (channel, title), so an episode already
             * indexed (normal download or a previous recovery) is skipped. */
            bool r_added = local_model_add(app, canon, col, ch_title,
                                fnv1a_positive(aac_path), ep_title, aac_path, dur,
                                next_download_seq(app), NULL);
            printf("[DBG] recover: file='%s' dur=%d added=%d\n", aac_path, dur, r_added);
            fflush(stdout);
            if (r_added) added_any = true;
        }
        closedir(cd);

        if (added_any) local_write_channel(app, canon);
    }
    closedir(root);
}

/* One-time: split the legacy monolithic .meta.json into per-channel buckets. */
static void migrate_old(struct PodcastApp *app) {
    char *buf = read_file(DL_META_OLD);
    if (!buf) { remove(DL_META_OLD); return; }

    const char *ep = buf;
    for (;;) {
        const char *obj = strstr(ep, "{\"cid\":");
        if (!obj) break;
        ep = obj + 1;
        int cid = json_get_int(obj, "cid", 0);
        int col = json_get_int(obj, "col", 0);
        int canon = (col > 0) ? col : cid;
        int eid = json_get_int(obj, "eid", 0);
        int dur = json_get_int(obj, "dur", 0);
        static char ch_title[256], eptitle[256], url[1024];
        json_get_str(obj, "ch",  ch_title, sizeof(ch_title));
        json_get_str(obj, "ep",  eptitle,  sizeof(eptitle));
        json_get_str(obj, "url", url,      sizeof(url));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur, 0, NULL);
    }
    free(buf);

    for (int i = 0; i < app->model->local_channel_count; i++)
        local_write_channel(app, app->model->local_channels[i].id);
    remove(DL_META_OLD);   /* buckets are now the source of truth */
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool cache_local_has_episode(struct PodcastApp *app, int episode_id) {
    if (!app || !app->model || episode_id <= 0) return false;
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++)
        if (m->local_episodes[i].id == episode_id) return true;
    return false;
}

bool cache_local_has_path(struct PodcastApp *app, const char *path) {
    if (!app || !app->model || !path || !path[0]) return false;
    return local_model_has_path(app, path);
}

/* Drop local-list entries whose audio file is missing/empty/truncated.  Runs
 * after load + recovery so a failed download (or an earlier recovery that
 * indexed a partial file) doesn't leave a broken card in the Local page. */
static void prune_incomplete_local(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    if (!m) return;

    bool changed = false;
    int write = 0;
    for (int i = 0; i < m->local_episode_count; i++) {
        Episode *e = &m->local_episodes[i];
        const Channel *ch = NULL;
        for (int j = 0; j < m->local_channel_count; j++)
            if (m->local_channels[j].id == e->channel_id) { ch = &m->local_channels[j]; break; }
        if (!ch) { changed = true; continue; }   /* orphaned episode */

        char path[1536];
        local_audio_path(ch->title, e->title, path, sizeof(path));
        /* Cheap existence/size check — a full mp4_probe/aac_file_probe would
         * re-read every download on each app open, stalling the UI thread for
         * minutes.  Duration is already persisted at download/remux time, so it
         * needs no refreshing here. */
        struct stat st;
        bool complete = (stat(path, &st) == 0 && st.st_size > 0);
        if (complete) {
            if (write != i) m->local_episodes[write] = m->local_episodes[i];
            write++;
        } else {
            printf("[DBG] prune: DROP id=%d ch='%s'(id=%d) ep='%s' path=%s\n",
                   e->id, ch->title, ch->id, e->title, path);
            fflush(stdout);
            remove(path);   /* free the dead bytes */
            changed = true;
        }
    }
    m->local_episode_count = write;
    if (write > 0) {
        Episode *ne = realloc(m->local_episodes, write * sizeof(Episode));
        if (ne) m->local_episodes = ne;
    } else {
        free(m->local_episodes);
        m->local_episodes = NULL;
    }

    /* Recompute channel episode counts; drop channels left with none. */
    for (int j = 0; j < m->local_channel_count; j++) m->local_channels[j].episode_count = 0;
    for (int i = 0; i < m->local_episode_count; i++)
        for (int j = 0; j < m->local_channel_count; j++)
            if (m->local_channels[j].id == m->local_episodes[i].channel_id)
                m->local_channels[j].episode_count++;

    int cw = 0;
    for (int j = 0; j < m->local_channel_count; j++) {
        if (m->local_channels[j].episode_count > 0) {
            if (cw != j) m->local_channels[cw] = m->local_channels[j];
            cw++;
        } else {
            char meta[320];
            channel_file_path(meta, sizeof(meta), m->local_channels[j].id);
            remove(meta);
            changed = true;
        }
    }
    m->local_channel_count = cw;
    if (cw > 0) {
        Channel *nc = realloc(m->local_channels, cw * sizeof(Channel));
        if (nc) m->local_channels = nc;
    } else {
        free(m->local_channels);
        m->local_channels = NULL;
    }

    if (changed)
        for (int j = 0; j < m->local_channel_count; j++)
            local_write_channel(app, m->local_channels[j].id);
}

void cache_local_init(struct PodcastApp *app) {
    if (!app || !app->model) return;
    downloads_ensure_dir();

    struct stat st;
    if (stat(DL_META_OLD, &st) == 0) migrate_old(app);   /* upgrade legacy monolith once */
    else                            load_buckets(app);

    /* Recover audio files whose index was lost (power-cut before flush). */
    recover_orphaned_downloads(app);

    /* Drop any recovered/loaded entry whose file is no longer complete. */
    prune_incomplete_local(app);

    if (app->model->local_episode_count > 0) {
        app->model->local_has_content = true;
        app->model->local_sd_mounted  = true;
    }

    /* DEBUG: dump final local model */
    printf("[DBG] cache_init: %d channels, %d episodes\n",
           app->model->local_channel_count, app->model->local_episode_count);
    for (int i = 0; i < app->model->local_channel_count; i++) {
        Channel *c = &app->model->local_channels[i];
        printf("[DBG]   ch id=%d col=%d epc=%d title='%s'\n",
               c->id, c->collection_id, c->episode_count, c->title);
    }
    for (int i = 0; i < app->model->local_episode_count; i++) {
        Episode *e = &app->model->local_episodes[i];
        printf("[DBG]   ep id=%d ch=%d dur=%d title='%s'\n",
               e->id, e->channel_id, e->duration_sec, e->title);
    }
    fflush(stdout);
}

void cache_local_add(struct PodcastApp *app,
                     int cid, const char *ch_title,
                     int eid, const char *ep_title,
                     const char *url, int dur,
                     const char *path, int collection_id,
                     const char *pub_date) {
    if (!app || !app->model) return;
    downloads_ensure_dir();

    /* Stable channel identity = collection_id (falls back to the transient cid). */
    /* The feed's itunes:duration can be wrong (mis-tagged episode). For a
     * completed local download, trust the real duration. The normal completion
     * path already computed it during the remux (the worker persisted it into
     * the task's duration_sec), so `dur` is authoritative there — re-probing the
     * whole .aac here (aac_file_probe scans every frame) was a full-file read on
     * the UI thread that doubled the post-download I/O and made the task appear
     * stuck at 100%. Only fall back to a full probe when we don't already have a
     * valid duration (recovery/backfill of a legacy record). */
    if (path && path[0]) {
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0) {
            printf("[DBG] cache_local_add: ep='%s' file missing/empty -> skip\n",
                   ep_title ? ep_title : "");
            fflush(stdout);
            /* The recorded path has no file behind it — a stale/truncated task
             * record (or a failed remux). Adding it anyway forks a duplicate
             * "identical" episode for the same real file (the 资讯早7点 bug).
             * Recovery already indexes every real on-SD file, so skip instead of
             * inserting a broken/duplicate row. */
            return;
        }
        if (dur <= 0) {
            int real_dur = cache_local_file_duration(path);
            if (real_dur > 0) {
                printf("[DBG] cache_local_add: ep='%s' probed dur=%d\n",
                       ep_title ? ep_title : "", real_dur);
                fflush(stdout);
                dur = real_dur;
            } else {
                printf("[DBG] cache_local_add: ep='%s' probe fail (dur=%d) -> skip\n",
                       ep_title ? ep_title : "", dur);
                fflush(stdout);
                return;
            }
        }
    }

    int canon = (collection_id > 0) ? collection_id : cid;
    bool added = local_model_add(app, canon, collection_id, ch_title, eid, ep_title,
                                 url, dur, next_download_seq(app), pub_date);

    app->model->local_has_content = true;
    app->model->local_sd_mounted  = true;

    if (added) local_write_channel(app, canon);   /* rewrite only this channel's small file */

    FILE *logf = fopen(DL_LOG_PATH, "a");
    if (logf) {
        time_t now = time(NULL);
        char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(logf, "[%s] cid=%d ch='%s' eid=%d ep='%s' dur=%ds\n",
                ts, cid, ch_title ? ch_title : "", eid, ep_title ? ep_title : "", dur);
        fclose(logf);
    }
}

void cache_local_update_duration(struct PodcastApp *app, int episode_id, int duration_sec)
{
    if (!app || !app->model || episode_id <= 0 || duration_sec <= 0) return;
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++) {
        Episode *e = &m->local_episodes[i];
        if (e->id != episode_id) continue;
        if (e->duration_sec == duration_sec) return;   /* already correct */
        e->duration_sec = duration_sec;
        local_write_channel(app, e->channel_id);       /* persist to the bucket JSON */
        return;
    }
}

void cache_local_update_pub_date(struct PodcastApp *app, int episode_id, const char *pub_date)
{
    if (!app || !app->model || episode_id <= 0 || !pub_date || !pub_date[0]) return;
    PodcastModel *m = app->model;
    for (int i = 0; i < m->local_episode_count; i++) {
        Episode *e = &m->local_episodes[i];
        if (e->id != episode_id) continue;
        if (strcmp(e->pub_date, pub_date) == 0) return;   /* already correct */
        snprintf(e->pub_date, sizeof(e->pub_date), "%s", pub_date);
        local_write_channel(app, e->channel_id);          /* persist to the bucket JSON */
        return;
    }
}

bool cache_local_remove_episode(struct PodcastApp *app, int episode_id)
{
    if (!app || !app->model || episode_id <= 0) return false;
    PodcastModel *m = app->model;

    int idx = -1;
    for (int i = 0; i < m->local_episode_count; i++)
        if (m->local_episodes[i].id == episode_id) { idx = i; break; }
    if (idx < 0) return false;

    Episode *e = &m->local_episodes[idx];
    int canon = e->channel_id;

    const Channel *ch = NULL;
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == canon) { ch = &m->local_channels[i]; break; }

    /* Delete the audio file (best-effort). */
    if (ch) {
        char audio_path[2048];
        local_audio_path(ch->title, e->title, audio_path, sizeof(audio_path));
        remove(audio_path);
        printf("[INF] remove_episode: %s\n", audio_path); fflush(stdout);
    }

    /* Drop the episode from the in-RAM model. */
    memmove(&m->local_episodes[idx], &m->local_episodes[idx + 1],
            (m->local_episode_count - idx - 1) * sizeof(Episode));
    m->local_episode_count--;

    /* Decrement the owning channel's episode count; drop the channel (and its
     * metadata bucket + audio directory) once it has no episodes left. */
    bool channel_removed = false;
    for (int i = 0; i < m->local_channel_count; i++) {
        if (m->local_channels[i].id != canon) continue;
        m->local_channels[i].episode_count--;
        if (m->local_channels[i].episode_count <= 0) {
            char meta[320];
            channel_file_path(meta, sizeof(meta), canon);
            remove(meta);

            char ch_dir[1536];
            char ch_safe[256];
            snprintf(ch_safe, sizeof(ch_safe), "%s", m->local_channels[i].title);
            for (char *p = ch_safe; *p; p++)
                if (strchr("\\/:*?\"<>|", *p)) *p = '_';
            snprintf(ch_dir, sizeof(ch_dir), DL_BASE_PATH "/%s", ch_safe);
            remove(ch_dir);   /* best-effort; FATFS fails if dir not empty */

            memmove(&m->local_channels[i], &m->local_channels[i + 1],
                    (m->local_channel_count - i - 1) * sizeof(Channel));
            m->local_channel_count--;
            channel_removed = true;
        }
        break;
    }

    /* Persist the reduced bucket (or leave the already-deleted bucket). */
    if (!channel_removed)
        local_write_channel(app, canon);

    m->local_has_content = (m->local_channel_count > 0);
    return true;
}
