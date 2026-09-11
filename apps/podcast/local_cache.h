/**
 * @file local_cache.h
 * @brief Local download library index — one JSON file per channel, bucketed
 *        under .nomadcast/downloads/.meta/<collection_id>.json
 */
#ifndef LOCAL_CACHE_H
#define LOCAL_CACHE_H

#include <stdbool.h>

struct PodcastApp;

void cache_local_init(struct PodcastApp *app);
void cache_local_add(struct PodcastApp *app,
                     int channel_id, const char *channel_title,
                     int episode_id, const char *episode_title,
                     const char *audio_url, int duration_sec,
                     const char *file_path, int collection_id);

/** True if an episode with this id is already in the local library index.
 *  Used to dedup the boot-time backfill from completed download tasks. */
bool cache_local_has_episode(struct PodcastApp *app, int episode_id);

/** Adopt a decoder-reported real duration into the local library index and
 *  rewrite the owning channel's metadata bucket.  No-op if the episode id is
 *  not in the local library (e.g. it was a network stream, not a download). */
void cache_local_update_duration(struct PodcastApp *app, int episode_id, int duration_sec);

/** Remove one downloaded episode: delete its audio file on SD, drop it from the
 *  local library model, and rewrite (or delete) the owning channel's metadata
 *  bucket. If the channel is left with no episodes it is removed too.
 *  Returns true if the episode was found and removed. */
bool cache_local_remove_episode(struct PodcastApp *app, int episode_id);

/** True if the on-SD file at `path` is a complete, playable M4A (has a moov
 *  atom).  False for missing/empty/truncated files.  Used to mark a network
 *  episode as "downloaded" without exposing the box parser. */
bool cache_local_file_complete(const char *path);

/** Read the real playback duration (seconds) from a local M4A's moov→mvhd atom.
 *  Returns 0 if the file is missing/incomplete/unknown.  The feed's
 *  itunes:duration can be wrong, so this is the authoritative duration for a
 *  downloaded file. */
int cache_local_file_duration(const char *path);

#endif
