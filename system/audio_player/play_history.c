/*
 * NomadCast — Local Playback History (see play_history.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"

#include "play_history.h"

static const char *TAG = "play_history";

#define HISTORY_DIR  "/sdcard/.nomadcast"
#define HISTORY_PATH "/sdcard/.nomadcast/play_history.json"

static play_history_entry_t s_entries[PLAY_HISTORY_MAX];
static int  s_count  = 0;
static bool s_loaded = false;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void ensure_dir(void) {
    struct stat st;
    if (stat(HISTORY_DIR, &st) == 0) return;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", HISTORY_DIR);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void json_esc(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
}

static bool entry_from_line(const char *line, play_history_entry_t *e) {
    memset(e, 0, sizeof(*e));

    const char *k = strstr(line, "\"path\":\"");
    if (!k) return false;
    k += strlen("\"path\":\"");
    int i = 0;
    while (*k && *k != '"' && i < (int)sizeof(e->path) - 1) {
        if (*k == '\\') k++;
        if (*k) e->path[i++] = *k++;
    }
    e->path[i] = '\0';
    if (!e->path[0]) return false;

    k = strstr(line, "\"byte\":");
    if (k) e->resume_byte = (int64_t)atoll(k + strlen("\"byte\":"));
    k = strstr(line, "\"sec\":");
    if (k) e->resume_sec = atoi(k + strlen("\"sec\":"));
    k = strstr(line, "\"ts\":");
    if (k) e->last_updated = (int64_t)atoll(k + strlen("\"ts\":"));
    return true;
}

/* Strip the trailing extension in place (basename ".aac"/".m4a"/".mp3"…), so
 * entries that differ only by extension compare equal — the same episode. */
static void strip_ext(char *p) {
    char *dot = strrchr(p, '.');
    char *slash = strrchr(p, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
}

/* Drop dead entries and collapse duplicates.  When the local library was
 * remuxed MP4→ADTS, every downloaded file moved from "<ep>.m4a" to "<ep>.aac";
 * the old ".m4a" play-history entries were left behind and — because the
 * Recently Played page titles them by basename-minus-extension — showed the
 * same episode twice.  A ".m4a" whose file is gone is a stale entry; two
 * entries whose paths differ only by extension are the same episode.  Entries
 * arrive most-recent-first, so the first one kept wins. */
static void prune_history(void) {
    int w = 0;
    for (int i = 0; i < s_count; i++) {
        play_history_entry_t *e = &s_entries[i];

        struct stat st;
        if (stat(e->path, &st) != 0) continue;   /* file gone → drop */

        char base[PLAY_HISTORY_PATH_MAX];
        snprintf(base, sizeof(base), "%s", e->path);
        strip_ext(base);

        bool dup = false;
        for (int j = 0; j < w; j++) {
            char ex[PLAY_HISTORY_PATH_MAX];
            snprintf(ex, sizeof(ex), "%s", s_entries[j].path);
            strip_ext(ex);
            if (strcmp(base, ex) == 0) { dup = true; break; }
        }
        if (dup) continue;

        if (w != i) s_entries[w] = s_entries[i];
        w++;
    }
    s_count = w;
}

static void ensure_loaded(void) {
    if (s_loaded) return;
    s_loaded = true;
    s_count  = 0;

    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) return;

    char line[1024];
    while (s_count < PLAY_HISTORY_MAX && fgets(line, sizeof(line), f)) {
        play_history_entry_t e;
        if (entry_from_line(line, &e))
            s_entries[s_count++] = e;
    }
    fclose(f);
    prune_history();
    ESP_LOGI(TAG, "Loaded %d playback-history entries", s_count);
}

/* ── public API ───────────────────────────────────────────────────────────── */

void play_history_init(void) { ensure_loaded(); }

void play_history_save(void) {
    ensure_loaded();
    ensure_dir();

    /* Direct write + fsync — FatFs's tmp+rename leaves the file invisible after
     * a power-cut (same lesson as task_store.c). */
    FILE *f = fopen(HISTORY_PATH, "w");
    if (!f) { ESP_LOGW(TAG, "open for write failed"); return; }

    for (int i = 0; i < s_count; i++) {
        play_history_entry_t *e = &s_entries[i];
        fprintf(f, "{\"path\":\"");
        json_esc(f, e->path);
        fprintf(f, "\",\"byte\":%lld,\"sec\":%d,\"ts\":%lld}\n",
                (long long)e->resume_byte, e->resume_sec,
                (long long)e->last_updated);
    }

    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) unlink(HISTORY_PATH);
}

void play_history_upsert(const char *path, int64_t resume_byte, int resume_sec) {
    if (!path || !path[0]) return;
    ensure_loaded();

    /* Move-to-front when already present. */
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, path) == 0) {
            play_history_entry_t e = s_entries[i];
            memmove(&s_entries[1], &s_entries[0], i * sizeof(play_history_entry_t));
            s_entries[0] = e;
            s_entries[0].resume_byte  = resume_byte;
            s_entries[0].resume_sec   = resume_sec;
            s_entries[0].last_updated = (int64_t)time(NULL);
            return;
        }
    }

    /* New entry: insert at front, evict oldest if full. */
    if (s_count < PLAY_HISTORY_MAX) s_count++;
    memmove(&s_entries[1], &s_entries[0],
            (s_count - 1) * sizeof(play_history_entry_t));
    memset(&s_entries[0], 0, sizeof(play_history_entry_t));
    snprintf(s_entries[0].path, sizeof(s_entries[0].path), "%s", path);
    s_entries[0].resume_byte  = resume_byte;
    s_entries[0].resume_sec   = resume_sec;
    s_entries[0].last_updated = (int64_t)time(NULL);
}

void play_history_remove(const char *path) {
    if (!path || !path[0]) return;
    ensure_loaded();

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, path) == 0) {
            int tail = s_count - i - 1;
            if (tail > 0)
                memmove(&s_entries[i], &s_entries[i + 1],
                        tail * sizeof(play_history_entry_t));
            s_count--;
            return;
        }
    }
}

bool play_history_find(const char *path, play_history_entry_t *out) {
    if (!path || !path[0]) return false;
    ensure_loaded();

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, path) == 0) {
            if (out) *out = s_entries[i];
            return true;
        }
    }
    return false;
}

int play_history_count(void) { ensure_loaded(); return s_count; }

bool play_history_get(int i, play_history_entry_t *out) {
    ensure_loaded();
    if (i < 0 || i >= s_count || !out) return false;
    *out = s_entries[i];
    return true;
}
