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

/* Add one episode to the model, keyed by canonical channel id. Dedups the
 * episode by stable audio_url and the channel by canon. Returns true if a NEW
 * episode was inserted (so the caller knows to persist that channel's bucket). */
static bool local_model_add(struct PodcastApp *app, int canon, int col,
                            const char *ch_title, int eid, const char *ep_title,
                            const char *url, int dur) {
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

    int en = m->local_episode_count + 1;
    Episode *ne = (Episode *)realloc(m->local_episodes, en * sizeof(Episode));
    if (!ne) return false;
    m->local_episodes = ne;
    Episode *e = &m->local_episodes[m->local_episode_count];
    memset(e, 0, sizeof(Episode));
    e->id = eid; e->channel_id = canon; e->duration_sec = dur;
    snprintf(e->title, sizeof(e->title), "%s", ep_title ? ep_title : "");
    snprintf(e->audio_url, sizeof(e->audio_url), "%s", url ? url : "");
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
        fprintf(f, ",\"dur\":%d}", e->duration_sec);
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
    char ch_title[128];
    json_get_str(buf, "ch", ch_title, sizeof(ch_title));
    if (canon <= 0) return;

    const char *ep = buf;
    for (;;) {
        const char *obj = strstr(ep, "{\"eid\":");
        if (!obj) break;
        ep = obj + 1;
        int eid = json_get_int(obj, "eid", 0);
        int dur = json_get_int(obj, "dur", 0);
        static char eptitle[256], url[1024];   /* boot-time, single-threaded */
        json_get_str(obj, "ep",  eptitle, sizeof(eptitle));
        json_get_str(obj, "url", url, sizeof(url));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur);
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
    snprintf(ep_safe, sizeof(ep_safe), "%s.m4a", ep_title ? ep_title : "audio");
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
 * "downloaded" (green check) without exposing the box parser. */
bool cache_local_file_complete(const char *path) {
    return mp4_probe(path, NULL);
}

/* Recover downloaded audio whose .meta bucket was lost (power cut before the
 * directory flush, or an SD card migrated without its index), or whose download
 * failed after the file was fully written (the task never registered the
 * episode). Scans the downloads tree and re-indexes any complete .m4a file
 * missing from the local model — including individual episodes of a channel
 * that already exists. Metadata that can't be recovered from a filename (CDN
 * url, duration, collection id) is left empty/zero; playback still works
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
            if (len < 5 || strcmp(fde->d_name + len - 4, ".m4a") != 0) continue;

            /* Index only complete M4A files — skip empty and truncated ones.
             * mp4_probe verifies the boxes tile to EOF, so a partial download
             * (truncated mdat) is rejected even when moov sits at the front. */
            char fpath[640];
            snprintf(fpath, sizeof(fpath), "%s/%s", chdir, fde->d_name);
            int dur = 0;
            if (!mp4_probe(fpath, &dur)) continue;

            char ep_title[256];
            snprintf(ep_title, sizeof(ep_title), "%.*s", (int)(len - 4), fde->d_name);

            /* local_model_add dedups by (channel, title), so an episode already
             * indexed (normal download or a previous recovery) is skipped. */
            bool r_added = local_model_add(app, canon, col, ch_title,
                                fnv1a_positive(fpath), ep_title, fpath, dur);
            printf("[DBG] recover: file='%s' dur=%d added=%d\n", fpath, dur, r_added);
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
        static char ch_title[128], eptitle[256], url[1024];
        json_get_str(obj, "ch",  ch_title, sizeof(ch_title));
        json_get_str(obj, "ep",  eptitle,  sizeof(eptitle));
        json_get_str(obj, "url", url,      sizeof(url));
        local_model_add(app, canon, col, ch_title, eid, eptitle, url, dur);
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
        if (mp4_probe(path, NULL)) {
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
        printf("[DBG]   ep id=%d ch=%d title='%s'\n", e->id, e->channel_id, e->title);
    }
    fflush(stdout);
}

void cache_local_add(struct PodcastApp *app,
                     int cid, const char *ch_title,
                     int eid, const char *ep_title,
                     const char *url, int dur,
                     const char *path, int collection_id) {
    (void)path;
    if (!app || !app->model) return;
    downloads_ensure_dir();

    /* Stable channel identity = collection_id (falls back to the transient cid). */
    int canon = (collection_id > 0) ? collection_id : cid;
    bool added = local_model_add(app, canon, collection_id, ch_title, eid, ep_title, url, dur);

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
