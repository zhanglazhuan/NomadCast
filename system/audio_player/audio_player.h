/*
 * NomadCast Audio Player — thin wrapper around ESP-ADF pipeline
 *
 * Pipeline: http_stream → (aac|mp3)_decoder → i2s_stream
 *
 * Usage:
 *   audio_player_play("https://example.com/podcast.m4a");
 *   audio_player_stop();
 *   audio_player_set_volume(80);
 */
#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize audio hardware (I2C, ES8156, I2S). Call once at boot. */
bool audio_player_init(void);

/** Attach the ES8156 codec on the shared hardware I2C bus (created by GT911)
 *  and apply the saved volume. Call once at boot, after gt911_init(). */
void audio_player_codec_init(i2c_master_bus_handle_t i2c_bus);

/** Start headphone jack-detect monitoring (AMP_EN GPIO18 → AP_EN GPIO44).
 *  Call once at boot (audio_player_codec_init does it). Inserting a headphone
 *  mutes the speaker amp; unplugging restores it. */
void audio_player_headphone_detect_init(void);

/** Current volume 0-100. */
int audio_player_get_volume(void);

/** Current playback position in seconds (wall-clock, pause-aware; 0 when idle). */
int audio_player_get_position_sec(void);

/** Start streaming from URL. Non-blocking — runs in a FreeRTOS task. */
bool audio_player_play(const char *url);

/** Stop playback and free pipeline resources. */
void audio_player_stop(void);

/** Pause / resume current playback. */
void audio_player_pause(bool pause);

/** Set volume 0-100. */
void audio_player_set_volume(int vol);

/** Is audio currently playing? */
bool audio_player_is_playing(void);

/** Is the audio pipeline alive (playing OR paused OR mid-teardown)?
 *  True from play() until the pipeline is fully torn down and its memory freed.
 *  Use this (not is_playing) to decide when the internal DRAM is actually free
 *  again — is_playing goes false on pause/at natural end before teardown. */
bool audio_player_is_active(void);
/** True when internal DRAM is too fragmented/low to safely start a download. */
bool audio_player_memory_pressure(void);
bool audio_player_is_local_source(void);
void audio_player_log_memory(const char *where);

/** Release pipeline memory without losing position (for download task).
 *  After this, pause(false) will rebuild and seek to the saved position.
 *  Safe to call even when not playing — no-op if pipeline is already gone. */
void audio_player_release(void);

#ifdef __cplusplus
}
#endif

#endif
