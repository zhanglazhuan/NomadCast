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
#include "driver/i2c_master.h"   /* i2c_master_bus_handle_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize audio hardware (I2C, ES8156, I2S). Call once at boot. */
bool audio_player_init(void);

/** Attach the ES8156 codec on the given (shared) I2C bus and apply the saved
 *  volume. Call once at boot after the I2C bus exists. */
void audio_player_codec_init(i2c_master_bus_handle_t bus);

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

#ifdef __cplusplus
}
#endif

#endif
