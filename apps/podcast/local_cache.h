/**
 * @file local_cache.h
 * @brief Local download library index — one JSON file per channel, bucketed
 *        under .podcast/downloads/.meta/<collection_id>.json
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

#endif
