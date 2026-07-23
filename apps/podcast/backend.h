/**
 * @file backend.h — Podcast backend API (Channel/Episode terminology)
 */
#ifndef PODCAST_BACKEND_H
#define PODCAST_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Podcast proxy server — change this to the production cloud IP */
#define PODCAST_SERVER  "http://192.168.137.1:5000"

#define BK_MAX_TITLE    256
#define BK_MAX_ARTIST   128
#define BK_MAX_URL      2048
#define BK_MAX_DESC     1024

/** A podcast channel (show) */
typedef struct {
    int     collection_id;
    char    title[BK_MAX_TITLE];
    char    artist[BK_MAX_ARTIST];
    char    feed_url[BK_MAX_URL];
    char    artwork_url[BK_MAX_URL];
    char    genre[64];
    int     episode_count;
    char    release_date[32];
} bk_channel_t;

/** Legacy alias */
// removed legacy alias

/** A single episode */
typedef struct {
    long long track_id;
    char    title[BK_MAX_TITLE];
    char    audio_url[BK_MAX_URL];
    char    audio_type[64];
    long long audio_length;
    int     duration_ms;
    char    pub_date[128];
    char    description[BK_MAX_DESC];
    char    artwork_url[BK_MAX_URL];
    int     collection_id;
} bk_episode_t;

/** Search results */
typedef struct {
    bk_channel_t  *channels;
    int            channel_count;
    bk_episode_t  *episodes;
    int            episode_count;
} bk_search_result_t;

typedef bk_search_result_t bk_search_result_t_legacy; /* unused */

/** Chart result */
typedef struct {
    bk_channel_t *channels;
    int           channel_count;
    char          title[BK_MAX_TITLE];
    char          country[8];
    char          updated[64];
} bk_chart_result_t;

/** Genre info */
typedef struct {
    int   genre_id;
    char  name[128];
    char  parent[128];
} bk_genre_t;

typedef struct {
    bk_genre_t *genres;
    int         genre_count;
} bk_genre_list_t;

/** Parsed RSS feed */
typedef struct {
    char    title[BK_MAX_TITLE];
    char    description[BK_MAX_DESC];
    char    image_url[BK_MAX_URL];
    char    link[BK_MAX_URL];
    bk_episode_t *episodes;
    int           episode_count;
} bk_feed_t;

/** Download result */
typedef struct {
    bool  success;
    char  file_path[BK_MAX_URL];
    int   file_size;
    char  error[256];
    void *user_data;
} bk_download_result_t;

/* ── Callback types ──────────────────────────────────────────────────── */

typedef void (*bk_search_cb_t)(const bk_search_result_t *result, void *user_data);
typedef void (*bk_chart_cb_t)(const bk_chart_result_t *result, void *user_data);
typedef void (*bk_genres_cb_t)(const bk_genre_list_t *result, void *user_data);
typedef void (*bk_channel_cb_t)(const bk_channel_t *channel, void *user_data);
typedef void (*bk_channels_cb_t)(bk_channel_t *channels, int count, void *user_data);
typedef void (*bk_feed_cb_t)(const bk_feed_t *feed, void *user_data);
typedef void (*bk_download_cb_t)(const bk_download_result_t *result);
typedef void (*bk_error_cb_t)(int status_code, const char *error, void *user_data);

/* Legacy aliases */
typedef bk_channel_cb_t bk_channel_cb_t;
typedef bk_channels_cb_t bk_channels_cb_t;

/* ── API ──────────────────────────────────────────────────────────────── */

void backend_init(void);
void backend_deinit(void);

/** Fetch top podcasts chart. genre_id=0 for all, or Apple podcast genre ID. */
int backend_fetch_chart(bk_channel_t **out, const char *country, int limit, int genre_id);

/** Search Apple Podcasts. Returns malloc'd bk_channel_t array. */
int backend_search_podcasts_sync(bk_channel_t **out, const char *keyword, const char *country, int limit);

/** Batch lookup channel details by collection IDs. Returns malloc'd bk_channel_t array. */
int backend_lookup_batch_sync(const int *ids, int count, const char *country, bk_channel_t **out);

/* ── Memory ───────────────────────────────────────────────────────────── */

void backend_search_result_free(bk_search_result_t *r);
void backend_chart_result_free(bk_chart_result_t *r);
void backend_genre_list_free(bk_genre_list_t *r);
void backend_feed_free(bk_feed_t *f);

#ifdef __cplusplus
}
#endif

#endif
