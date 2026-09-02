/**
 * @file task_store.h
 * @brief Download-task persistence — one JSON file per task on SD card.
 *
 * Directory layout:
 *   /sdcard/.nomadcast/cache/download_tasks/
 *       00000001.json    ← {"id":1, "ep":"...", "st":"pending", "ts":1750000000, ...}
 *       00000002.json    ← sorted by filename = download FIFO order
 *       .next_id         ← "3"  (next id to assign)
 *
 * Lifecycle:
 *   1. task_store_load()  — scan dir, filter ts > now-3d, populate model array
 *   2. task_store_create() — write new .json + append to model array
 *   3. task_store_update() — rewrite single .json on status change
 *   4. task_store_delete() — unlink + remove from model
 */
#ifndef TASK_STORE_H
#define TASK_STORE_H

#include <stdbool.h>
#include "model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_STORE_DIR  "/sdcard/.nomadcast/cache/download_tasks"
#define TASK_TTL_DAYS   3

/** Scan the directory and populate model->download_tasks[] with tasks
 *  created in the last TASK_TTL_DAYS.  Returns the loaded count. */
int  task_store_load(struct PodcastApp *app);

/** Create a new PENDING task — writes file + adds to model array.
 *  Returns the assigned task id, or -1 on error. */
int  task_store_create(struct PodcastApp *app,
                       const char *ep_title, const char *ch_title,
                       int duration_sec, const char *path,
                       const char *audio_url,
                       int episode_id, int channel_id, int collection_id);

/** Overwrite a single task file with current model state.
 *  Called after status changes (pending→downloading, downloading→complete). */
void task_store_update(struct PodcastApp *app, int task_id);

/** Delete a task — unlink file + compact the model array. */
void task_store_delete(struct PodcastApp *app, int task_id);

/** Purge tasks older than TASK_TTL_DAYS (called at boot). */
void task_store_purge_old(void);

#ifdef __cplusplus
}
#endif

#endif
