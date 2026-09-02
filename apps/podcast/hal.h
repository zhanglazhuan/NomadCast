/* ESP-IDF declarations for podcast app — real WiFi from hal_esp.c */
#ifndef PODCAST_HAL_STUB_H
#define PODCAST_HAL_STUB_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── WiFi ────────────────────────────────────────────────────────────────── */
/* Defined in apps/settings/hal_esp.c — shared by both apps */
bool hal_wifi_is_connected(void);
int  hal_wifi_get_disconnect_reason(void);

/* ── HTTP client (real impl in http_esp.c via esp_http_client) ──────────── */
void http_client_init(void);
void http_client_deinit(void);
char *http_get_sync(const char *url, int *status, int *len);
char *http_post_json_sync(const char *url, const char *json_body, int *status, int *len);
void http_free_response_body(void *body);
const char *http_last_error(void);  /* human-readable detail after http_get_sync returns NULL */

/* Streaming download to file — for audio downloads (PSRAM-friendly).
 * progress_cb (nullable) is invoked ~every 500ms with the running byte count
 * and the instantaneous speed in bytes/sec over that window.
 * Return false from the callback to ABORT the download: the transfer stops,
 * the partial file is unlinked, and http_download_to_file() returns false. */
typedef bool (*http_dl_progress_cb)(int bytes_done, int total_bytes, int speed_bps);
bool http_download_to_file(const char *url, const char *file_path,
                           http_dl_progress_cb progress_cb);

/* ── Audio playback (ESP32 only, through ADF pipeline) ──────────────────── */
#ifndef _WIN32
#include "audio_player.h"
static inline void hal_audio_play_file(const char *path) {
    audio_player_init();
    audio_player_play(path);
}
static inline void hal_audio_stop(void)  { audio_player_stop(); }
static inline void hal_audio_pause(bool pause) { audio_player_pause(pause); }
static inline int  hal_audio_get_position_sec(void) { return audio_player_get_position_sec(); }
#else
/* PC: implemented in pc_demo/hal.c */
bool hal_audio_play_file(const char *path);
void hal_audio_stop(void);
void hal_audio_pause(bool pause);
bool hal_audio_is_playing(void);
int  hal_audio_get_position_sec(void);
#endif

/* ── Stubs ────────────────────────────────────────────────────────────────── */
static inline void hal_http_download(const char *url, const char *path,
                                      void *ctx, int content_len) {
    (void)url; (void)path; (void)ctx; (void)content_len;
}
/* Real SD-mount check (defined in controller.c via FatFS f_getfree, same as the
 * Settings storage page). Returns non-zero when the SD card is mounted. */
int hal_sdcard_is_mounted(void);
#ifdef __cplusplus
}
#endif
#endif
