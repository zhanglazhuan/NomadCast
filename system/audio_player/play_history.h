/*
 * NomadCast — Local Playback History
 *
 * Records the last PLAY_HISTORY_MAX locally-played audio files with their
 * resume position (compressed byte offset + approximate seconds), so re-playing
 * a file after an exit resumes where it left off.
 *
 * Storage: /sdcard/.nomadcast/play_history.json (one JSON object per line,
 * most-recent-first).  Loaded lazily on first use; saved explicitly.
 *
 * Threading: call only from the LVGL / app task (internal-DRAM stack).  Never
 * call from an ADF element task — those run on PSRAM stacks and must not touch
 * NVS/flash (see audio_thread.c vendored patch note).
 */
#ifndef PLAY_HISTORY_H
#define PLAY_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#define PLAY_HISTORY_MAX      30
#define PLAY_HISTORY_PATH_MAX 256

typedef struct {
    char    path[PLAY_HISTORY_PATH_MAX]; /* local file path (key) */
    int64_t resume_byte;                 /* compressed byte offset to seek to */
    int     resume_sec;                  /* approximate seconds (UI only) */
    int64_t last_updated;                /* time(NULL) of last save */
} play_history_entry_t;

/** Load persisted history (idempotent; also done lazily by every accessor). */
void play_history_init(void);

/** Persist the in-memory history to SD. */
void play_history_save(void);

/** Insert (or move to front) an entry; evict oldest beyond PLAY_HISTORY_MAX. */
void play_history_upsert(const char *path, int64_t resume_byte, int resume_sec);

/** Remove an entry (e.g. a file finished near the end → restart from 0). */
void play_history_remove(const char *path);

/** Look up an entry by exact path. Returns true and fills *out when found. */
bool play_history_find(const char *path, play_history_entry_t *out);

/** Number of entries currently tracked (≤ PLAY_HISTORY_MAX). */
int play_history_count(void);

/** Read entry i (0 = most recent). Returns false when out of range. */
bool play_history_get(int i, play_history_entry_t *out);

#endif /* PLAY_HISTORY_H */
