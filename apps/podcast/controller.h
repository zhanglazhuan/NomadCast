/**
 * @file controller.h
 * @brief Podcast app controller — orchestrates async backend → model → view flows
 */
#ifndef PODCAST_CONTROLLER_H
#define PODCAST_CONTROLLER_H

#include "model.h"

struct PodcastApp;
struct PodcastModel;
struct PodcastView;

/* ── RSS feed types (was rss_parser.h; now inline since ESP32 doesn't use the RSS XML parser) ─── */

#define RSS_MAX_EPISODES    10   /* per-page buffer (== EPISODES_PER_PAGE) */
#define RSS_MAX_STR          512

/* Only the fields the proxy server actually returns (see
 * server/podcast_service/rss_parser.py). audio_type / audio_length / guid /
 * description are NOT sent — dropped here to keep per-page RAM low. */
typedef struct {
    char  title[RSS_MAX_STR];
    char  audio_url[RSS_MAX_STR];
    char  pub_date[128];
    char  duration[32];
} rss_episode_t;

typedef struct {
    char  title[RSS_MAX_STR];
    char  description[RSS_MAX_STR];
    char  image_url[RSS_MAX_STR];
    char  link[RSS_MAX_STR];
    rss_episode_t episodes[RSS_MAX_EPISODES];
    int  episode_count;
    int  total_episodes;
    char error[256];
} rss_feed_t;

/* ── Controller ────────────────────────────────────────────────────────────── */

typedef struct PodcastCtrlCtx PodcastCtrlCtx;  /* private, defined in controller.c */

typedef struct PodcastController {
    struct PodcastModel *model;
    struct PodcastView  *view;
    PodcastCtrlCtx      *ctx;        /* all mutable state — freed on deinit */
    bool                 loading_in_progress;
} PodcastController;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void podcast_controller_init(struct PodcastApp *app);
void podcast_controller_deinit(struct PodcastApp *app);

/* ── Network Content ─────────────────────────────────────────────────────── */

/**
 * @brief Load network content (top podcasts chart).
 *
 * Async: calls backend_get_top_podcasts() → model import → callback sets ready state.
 * View polls model->net_state to know when data is available.
 *
 * @return true if request was initiated, false if already loading or offline
 */
bool podcast_controller_load_network_content(struct PodcastApp *app);

/**
 * @brief Fetch top chart from Apple RSS, convert to Channel, import into model.
 *
 * Sync/blocking — calls backend_fetch_chart() with 3 retries, maps genres,
 * imports channels via podcast_model_import_channels(), and sets net_state.
 *
 * @return true if channels were loaded, false on failure.
 */
bool podcast_controller_fetch_chart(struct PodcastApp *app);

/** Fetch chart for a specific category (pass CHANNEL_CATEGORY_COUNT for all). */
bool podcast_controller_fetch_chart_by_category(struct PodcastApp *app, int cat);

/** Load more albums for a category */
bool podcast_controller_load_more_channels(struct PodcastApp *app, channel_category_t cat);

/* ── Search ──────────────────────────────────────────────────────────────── */

/**
 * @brief Search Apple Podcasts for albums + episodes.
 *
 * Async: backend_search_podcasts() + backend_search_episodes() → model search results.
 *
 * @param query  Search term
 * @return true if search initiated
 */
bool podcast_controller_search(struct PodcastApp *app, const char *query);

/* ── Channel Detail ────────────────────────────────────────────────────────── */

/**
 * @brief Fetch album episodes by parsing the RSS feed.
 *
 * Uses feed_url if available; otherwise looks up by collection_id first.
 *
 * @param album_id  Local album ID
 * @return true if fetch initiated
 */
bool podcast_controller_fetch_channel_episodes(struct PodcastApp *app, int album_id);

/* ── Login ───────────────────────────────────────────────────────────────── */

bool podcast_controller_login(struct PodcastApp *app,
                               const char *name, const char *password,
                               const char *confirm_pwd, bool registering, bool agreed,
                               const char **err_msg);

/* ── Local Content ───────────────────────────────────────────────────────── */

typedef enum {
    LOCAL_SD_MISSING,
    LOCAL_NO_CONTENT,
    LOCAL_HAS_CONTENT,
} local_content_state_t;

local_content_state_t podcast_controller_check_local_content(struct PodcastApp *app);

/** Delete all local audio files and metadata for a channel (SD-card clean-up).
 *  Called from the Local page after a left-swipe → confirm. */
void podcast_controller_delete_channel_local(struct PodcastApp *app, int channel_id);

/* ── Player ──────────────────────────────────────────────────────────────── */

/** Build the URL the ESP32 actually fetches for playback/download.
 *  Remote http(s) URLs are routed through the proxy server (which performs the
 *  TLS handshake and follows 302 redirects — the ESP32 only speaks plain HTTP
 *  to the proxy); local paths and already-proxied URLs are copied unchanged. */
void podcast_media_url(char *out, int out_sz, const char *url);
/** Route file downloads through /api/raw (byte forwarding, no transcoding). */
void podcast_download_url(char *out, int out_sz, const char *url);

/** Re-enqueue PENDING tasks after boot / WiFi reconnect */
void podcast_controller_resume_downloads(struct PodcastApp *app);

/** Pause all download activity (worker stays alive, waits for resume). */
void podcast_controller_pause_all_downloads(struct PodcastApp *app);

/** Resume downloads previously paused by the user. */
void podcast_controller_resume_all_downloads(struct PodcastApp *app);

/** True if the download worker is alive but user-paused. */
bool podcast_controller_is_download_paused(struct PodcastApp *app);

/** Play a track (sets up queue starting from this track) */
void podcast_controller_play_episode(struct PodcastApp *app, int track_id);

/** Play all tracks from an album */
void podcast_controller_play_channel(struct PodcastApp *app, int album_id);

/** Resolve the media URI to open for an episode: the downloaded local SD file if
 *  it exists, otherwise the network/proxy stream URL. Writes "" if eid unknown. */
void podcast_controller_media_for_episode(struct PodcastApp *app, int eid,
                                          char *out, int out_sz);

/** True if the episode can be played directly (downloaded local file, or a
 *  remote non-M4A stream). Remote M4A requires download first — server-side
 *  transcoding is disabled. Used by the list UI to skip the player page. */
bool podcast_controller_episode_playable(struct PodcastApp *app, int eid);

/** Toggle play/pause of the current episode (true ADF pause/resume). No-op if
 *  no episode is loaded. Safe to call off the LVGL thread. */
void podcast_controller_toggle_play_pause(struct PodcastApp *app);

/* ── Download ────────────────────────────────────────────────────────────── */

/** Start downloading an episode */
bool podcast_controller_download_episode(struct PodcastApp *app,
                                          const char *audio_url,
                                          const char *title,
                                          const char *channel_title);

/** Download with channel/episode IDs for local storage tracking */
bool podcast_controller_download_episode_ex(struct PodcastApp *app,
    const char *url, const char *title, const char *channel_title,
    int channel_id, int episode_id);

/** Download with duration for accurate metadata */
bool podcast_controller_download_episode_ex2(struct PodcastApp *app,
    const char *url, const char *title, const char *channel_title,
    int channel_id, int episode_id, int duration_sec, int collection_id);

/** Poll function: called periodically from LVGL timer to check async completion */
void podcast_controller_poll(struct PodcastApp *app);

/* ── Download ETA telemetry (measured throughput) ───────────────────────────
 * The download-task page divides remaining bytes by the measured link speed
 * instead of guessing from playback duration. All are safe to read from the
 * LVGL thread (single 32-bit atomic loads). */
/** Smoothed link throughput in bytes/sec (0 until the first live sample). */
int podcast_dl_avg_speed_bps(void);
/** task_id of the in-flight transfer, or -1 when idle. */
int podcast_dl_current_task_id(void);
/** Remaining bytes of the in-flight transfer (0 if idle or size unknown). */
int podcast_dl_current_remaining_bytes(void);
/** Learned file bytes per second of audio, for sizing queued tasks (0 if unknown). */
int podcast_dl_bytes_per_audio_sec(void);

/** Cancel pending/downloading tasks: abort the in-flight transfer, delete the
 *  partial file, drop queued items, and remove the persisted + in-memory record. */
void podcast_controller_cancel_download_tasks(struct PodcastApp *app,
                                              const int *task_ids, int count);

/** Remove completed/failed task records (persisted + in-memory) while KEEPING
 *  the downloaded audio file on disk. */
void podcast_controller_delete_download_records(struct PodcastApp *app,
                                                const int *task_ids, int count);

/* ── RSS fetch bridge (main loop ↔ album page) ────────────────────────── */

/** Call from main loop (outside lv_timer_handler) to process pending RSS fetch */
void controller_process_rss(void);

/** Call from main loop to process pending downloads */
void controller_process_download(void);

/** Channel page polls these to know when RSS fetch is done */
bool g_rss_done(void);
bool g_rss_ok(void);
rss_feed_t *g_rss_result(void);
int g_rss_channel_id(void);
void controller_rss_reparse(int offset, int limit);

#endif /* PODCAST_CONTROLLER_H */
