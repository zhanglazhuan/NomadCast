/**
 * @file mp4_remux.h
 * @brief Local MP4 (M4A) → AAC-ADTS remux so the ADF decoder can byte-seek.
 *
 * The precompiled ADF AAC/M4A decoder is a positional parser: MP4 carries frame
 * boundaries in the moov sample table and is consumed sequentially, so a
 * byte-offset resume into mdat errors out. ADTS, by contrast, has per-frame
 * 0xFFF sync words + a 13-bit frame_length, so the decoder resyncs to the next
 * frame after a reader seek — which is what audio_player's apply_local_byte_seek
 * needs (it deliberately excludes only .m4a/.mp4/.m4b, not .aac).
 */
#ifndef MP4_REMUX_H
#define MP4_REMUX_H

#include <stdbool.h>

/**
 * Convert an MP4/M4A container (single audio track) to a raw AAC-ADTS stream.
 * Parses moov→trak→mdia→minf→stbl (stsd/stsc/stsz/stco) and the esds→ASC to
 * build a correct 7-byte ADTS header per mdat sample.
 *
 * @param src             input .m4a path
 * @param dst             output .aac path (overwritten)
 * @param out_duration_sec if non-NULL, receives the real duration (0 if unknown)
 * @return true on success; dst is left absent/incomplete on failure.
 */
bool mp4_remux_to_adts(const char *src, const char *dst, int *out_duration_sec);

/** Raw byte copy src → dst (used as the fallback when the download is not MP4,
 *  e.g. an MP3 that is already seekable). Returns true on success. */
bool mp4_copy_file(const char *src, const char *dst);

/**
 * True if `path` is a complete AAC-ADTS stream (frames tile cleanly to EOF).
 * Fills *out_duration_sec (frames × 1024 / rate) when non-NULL.
 */
bool aac_file_probe(const char *path, int *out_duration_sec);

#endif
