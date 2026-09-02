/**
 * @file model.h
 * @brief Podcast app data model — Channel/Episode terminology (Apple Podcasts)
 */
#ifndef PODCAST_MODEL_H
#define PODCAST_MODEL_H

#include <stddef.h>
#include <time.h>

/* Global CJK font set by main.c — use instead of lv_font_montserrat_* */
struct _lv_font_t;
extern const struct _lv_font_t *g_cjk_font;

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct PodcastApp;

/* ── Download status ─────────────────────────────────────────────────────── */

typedef enum {
    DOWNLOAD_STATUS_PENDING = 0,
    DOWNLOAD_STATUS_DOWNLOADING,
    DOWNLOAD_STATUS_COMPLETED,
    DOWNLOAD_STATUS_FAILED,
} download_status_t;

/* ── Download task ───────────────────────────────────────────────────────── */

typedef struct {
    int     id;
    int     episode_id;
    int     channel_id;
    int     collection_id;
    char    episode_title[128];
    char    channel_title[128];
    download_status_t status;
    int     progress;
    int     duration_sec;
    time_t  created_at;          /* epoch — for 3-day filtering */
    char    file_path[512];      /* SD-card path to saved audio */
    char    audio_url[1024];     /* original CDN URL (for retry) */
} DownloadTask;

/* ── Channel categories ──────────────────────────────────────────────────── */

typedef enum {
    CHANNEL_CATEGORY_NEWS_SOCIETY = 0,   /* 新闻, 社会与文化 */
    CHANNEL_CATEGORY_BUSINESS_TECH,      /* 商务, 科技 */
    CHANNEL_CATEGORY_ARTS_HISTORY,       /* 艺术, 历史 */
    CHANNEL_CATEGORY_COMEDY_LIFE,        /* 喜剧, 休闲, 健康与健身 */
    CHANNEL_CATEGORY_CRIME_EDU,          /* 犯罪纪实, 教育 */
    CHANNEL_CATEGORY_OTHERS,             /* 其他未分类 */
    CHANNEL_CATEGORY_COUNT
} channel_category_t;

/* ── Channel (podcast show) ──────────────────────────────────────────────── */

typedef struct {
    int     id;
    int     collection_id;    /* Apple collectionId */
    char    title[256];
    char    artist[128];
    char    feed_url[2048];   /* RSS feed URL */
    char    artwork_url[2048];
    char    genre[64];
    int     episode_count;    /* total episodes in this channel */
    bool    downloaded;
    char    description[512];
    char    upload_time[64];
    int     play_count;
    channel_category_t category;
    uint32_t card_color;
} Channel;

/* ── Episode (single podcast episode) ───────────────────────────────────── */

typedef struct {
    int     id;
    int     channel_id;
    int     collection_id;
    long long track_id;       /* Apple trackId */
    char    title[256];
    char    audio_url[2048];
    char    audio_type[64];
    long long audio_length;
    int     duration_sec;
    char    description[512];
    char    pub_date[128];
} Episode;

/* ── Search results ──────────────────────────────────────────────────────── */

typedef struct {
    Channel *channels;
    int      channel_count;
    Episode *episodes;
    int      episode_count;
    char     query[64];
} SearchResults;

/* ── Network content state ───────────────────────────────────────────────── */

typedef enum {
    NET_STATE_IDLE,
    NET_STATE_LOADING,
    NET_STATE_READY,
    NET_STATE_ERROR,
    NET_STATE_OFFLINE,
} net_state_t;

/* ── Model ───────────────────────────────────────────────────────────────── */

typedef struct PodcastModel {
    Channel  *network_channels;
    int       network_channel_count;
    int       network_channels_shown[CHANNEL_CATEGORY_COUNT];
    int       network_active_category;  /* last selected category tab */

    Episode  *network_episodes;
    int       network_episode_count;

    net_state_t net_state;
    char    net_error[256];

    SearchResults search_results;

    Channel *current_channel;
    Episode *current_channel_episodes;
    int      current_channel_episode_count;
    int      current_channel_total_episodes;  /* total in RSS feed (may be > episode_count) */

    Channel *local_channels;
    int      local_channel_count;
    Episode *local_episodes;
    int      local_episode_count;
    bool     local_sd_mounted;
    bool     local_has_content;

    int    *queue;
    int     queue_count;
    int     queue_index;
    bool    player_playing;
    int     current_episode_id;

    char    search_history[10][64];
    int     search_history_count;

    DownloadTask *download_tasks;
    int           download_task_count;
    SemaphoreHandle_t download_mutex; /* protects task array vs worker */

    int     font_size;         /* 0=Small, 1=Medium, 2=Large */
    int     download_quality;  /* 0=Low(64k), 1=Med(128k), 2=High(320k) */

    bool    logged_in;
    char    username[64];
    char    password[64];      /* persisted to NVS */
    char    user_id[64];       /* server-generated: yyyymmddhhmmss-deviceID */
    char    device_id[32];     /* unique per device, persisted to NVS */
} PodcastModel;

#define NETWORK_PAGE_SIZE      10
#define EPISODES_PER_PAGE      10

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void podcast_model_init(struct PodcastApp *app);
void podcast_model_deinit(struct PodcastApp *app);
void podcast_model_download_lock(struct PodcastApp *app);
void podcast_model_download_unlock(struct PodcastApp *app);

/* ── Data import ─────────────────────────────────────────────────────────── */

void podcast_model_import_channels(struct PodcastApp *app, Channel *channels, int count);
void podcast_model_append_channels(struct PodcastApp *app, Channel *channels, int count);
void podcast_model_import_episodes(struct PodcastApp *app, Episode *episodes, int count);

void podcast_model_set_search_results(struct PodcastApp *app,
                                       Channel *channels, int channel_count,
                                       Episode *episodes, int episode_count,
                                       const char *query);
void podcast_model_clear_search_results(struct PodcastApp *app);
void podcast_model_set_net_state(struct PodcastApp *app, net_state_t state, const char *error);

/* ── Channel queries ─────────────────────────────────────────────────────── */

const Channel *podcast_model_get_channel_by_id(struct PodcastApp *app, int channel_id);
int podcast_model_get_channel_count_by_category(struct PodcastApp *app, channel_category_t cat);
const Channel **podcast_model_get_channels_by_category(struct PodcastApp *app, channel_category_t cat, int *out_count);

/* ── Episode queries ─────────────────────────────────────────────────────── */

const Episode *podcast_model_get_episode_by_id(struct PodcastApp *app, int episode_id);
const Episode **podcast_model_get_episodes_by_channel(struct PodcastApp *app, int channel_id, int *out_count);

/* ── Local content ───────────────────────────────────────────────────────── */

const Channel **podcast_model_get_downloaded_channels_by_category(struct PodcastApp *app, channel_category_t cat, int *out_count);
void podcast_model_set_local_state(struct PodcastApp *app, bool sd_mounted, bool has_content);
bool podcast_model_is_local_sd_mounted(struct PodcastApp *app);
bool podcast_model_has_local_content(struct PodcastApp *app);

/* ── Player queue ────────────────────────────────────────────────────────── */

void podcast_model_set_queue(struct PodcastApp *app, const int *episode_ids, int count);
int  podcast_model_queue_next(struct PodcastApp *app);
int  podcast_model_queue_prev(struct PodcastApp *app);
int  podcast_model_queue_current(struct PodcastApp *app);
void podcast_model_set_playing(struct PodcastApp *app, bool playing);
bool podcast_model_is_playing(struct PodcastApp *app);
int  podcast_model_get_current_episode_id(struct PodcastApp *app);

/* ── Search ──────────────────────────────────────────────────────────────── */

void podcast_model_add_search_history(struct PodcastApp *app, const char *query);
void podcast_model_clear_search_history(struct PodcastApp *app);
int  podcast_model_get_search_history_count(struct PodcastApp *app);
const char *podcast_model_get_search_history_item(struct PodcastApp *app, int index);

/* ── Download tasks ──────────────────────────────────────────────────────── */

const DownloadTask *podcast_model_get_download_tasks(struct PodcastApp *app, int *out_count);
int  podcast_model_get_pending_download_count(struct PodcastApp *app);
void podcast_model_delete_download_tasks(struct PodcastApp *app, const int *task_ids, int count);
void podcast_model_cancel_download_tasks(struct PodcastApp *app, const int *task_ids, int count);

/* ── Login ───────────────────────────────────────────────────────────────── */

void podcast_model_set_login(struct PodcastApp *app, const char *username, const char *password, const char *user_id);
void podcast_model_logout(struct PodcastApp *app);
bool podcast_model_is_logged_in(struct PodcastApp *app);
const char *podcast_model_get_username(struct PodcastApp *app);
const char *podcast_model_get_password(struct PodcastApp *app);
const char *podcast_model_get_user_id(struct PodcastApp *app);
const char *podcast_model_get_device_id(struct PodcastApp *app);

/* ── User settings ────────────────────────────────────────────────────────── */

int  podcast_model_get_font_size(struct PodcastApp *app);
void podcast_model_set_font_size(struct PodcastApp *app, int size);
int  podcast_model_get_download_quality(struct PodcastApp *app);
void podcast_model_set_download_quality(struct PodcastApp *app, int quality);


/* ── Network content ─────────────────────────────────────────────────────── */

bool podcast_model_load_more_network_channels(struct PodcastApp *app, channel_category_t cat);
int  podcast_model_get_network_shown(struct PodcastApp *app, channel_category_t cat);
int  podcast_model_get_network_active_category(struct PodcastApp *app);
void podcast_model_set_network_active_category(struct PodcastApp *app, int cat);
int  podcast_model_get_network_total(struct PodcastApp *app, channel_category_t cat);
const Channel **podcast_model_get_network_page(struct PodcastApp *app, channel_category_t cat,
                                                int offset, int limit, int *out_count);
net_state_t podcast_model_get_net_state(struct PodcastApp *app);
const char *podcast_model_get_net_error(struct PodcastApp *app);

/* ── Current channel detail ──────────────────────────────────────────────── */

void podcast_model_set_current_channel(struct PodcastApp *app, const Channel *channel,
                                        Episode *episodes, int episode_count);
const Channel *podcast_model_get_current_channel(struct PodcastApp *app);
const Episode *podcast_model_get_current_channel_episode(struct PodcastApp *app, int index);
int podcast_model_get_current_channel_episode_count(struct PodcastApp *app);

/* ── Compatibility wrappers ──────────────────────────────────────────────── */

static inline bool podcast_model_is_network_ready(struct PodcastApp *app) {
    return podcast_model_get_net_state(app) == NET_STATE_READY;
}
static inline const char *podcast_model_get_network_error(struct PodcastApp *app) {
    return podcast_model_get_net_error(app);
}
static inline void podcast_model_set_network_ready(struct PodcastApp *app) {
    podcast_model_set_net_state(app, NET_STATE_READY, NULL);
}
static inline void podcast_model_set_network_error(struct PodcastApp *app, const char *msg) {
    podcast_model_set_net_state(app, NET_STATE_ERROR, msg);
}

#endif /* PODCAST_MODEL_H */
