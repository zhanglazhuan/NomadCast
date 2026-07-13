/**
 * @file cache.h
 * @brief Podcast data cache — disk-backed JSON with TTL expiry
 *
 * Stores raw API responses in .podcast/cache/ with meta.json for TTL tracking.
 * All I/O is synchronous and expected to run before LVGL init.
 */
#ifndef PODCAST_CACHE_H
#define PODCAST_CACHE_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Cache entry types */
typedef enum {
    CACHE_CHART   = 0,   /* Apple Charts API raw JSON */
    CACHE_LOOKUP  = 1,   /* iTunes Lookup batch raw JSON */
    CACHE_COUNT
} cache_type_t;

#define CACHE_TTL_SECONDS  900   /* 15 minutes */

/** Initialize cache: create dirs, read meta.json, validate TTL */
void cache_init(void);

/** Check if a cached entry is still within TTL */
bool cache_is_valid(cache_type_t type);

/** Load cached entry. Returns malloc'd buffer (caller frees), NULL on failure. */
char *cache_load(cache_type_t type, int *out_len);

/** Save an entry to cache and update meta.json timestamp */
bool cache_save(cache_type_t type, const char *data, int len);

/** Invalidate a cache entry — marks stale in memory + meta.json */
void cache_invalidate(cache_type_t type);

/** Delete a corrupted cache entry — removes file AND marks stale */
void cache_purge(cache_type_t type);

/** Load cached lookup result for a collection_id (returns NULL if not cached/expired) */
char *cache_load_lookup_item(int collection_id, int *out_len);

/** Save lookup result for a collection_id to KV cache */
bool cache_save_lookup_item(int collection_id, const char *data, int len);

/* ── Artwork cache (.podcast/cache/artwork/) ──────────────────────────────── */

/**
 * @brief Download artwork from URL and save as PNG to SD card.
 * @param collection_id  iTunes collection ID (used as filename: <id>.png)
 * @param url            Artwork URL (e.g. artworkUrl600 from iTunes)
 * @return true on success
 */
bool cache_artwork_download(int collection_id, const char *url);

/**
 * @brief Check if artwork exists in cache for this collection_id.
 */
bool cache_artwork_exists(int collection_id);

/* ── Playback position (.podcast/playback.json) ────────────────────────── */

struct PodcastApp;

typedef struct {
    int     episode_id;
    int     channel_id;
    int     position_sec;
    bool    completed;
    int64_t last_updated;
} PlaybackEntry;

void cache_playback_init(struct PodcastApp *app);
void cache_playback_save(void);

PlaybackEntry *cache_playback_get(int episode_id);
bool cache_playback_is_completed(int episode_id);
int  cache_playback_get_position(int episode_id);

void cache_playback_set_position(int episode_id, int channel_id,
                                 int position_sec, int duration_sec);
void cache_playback_mark_completed(int episode_id, int channel_id,
                                   bool completed);

/** 0–100 progress for a channel card. */
int  cache_playback_channel_progress_pct(int channel_id, int total_episodes);

#ifdef __cplusplus
}
#endif

#endif /* PODCAST_CACHE_H */
