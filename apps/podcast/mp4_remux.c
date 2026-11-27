/**
 * @file mp4_remux.c
 * @brief MP4 (M4A) → AAC-ADTS remux (see mp4_remux.h).
 *
 * MP4 is big-endian; the ESP32 is little-endian, so every multi-byte field is
 * read via be32()/be64() from a raw byte buffer. Samples are iterated chunk by
 * chunk (stco chunk offsets × stsc samples-per-chunk), reading each sample's
 * length from stsz. Within a chunk the samples are contiguous in mdat, so only
 * one seek per chunk is needed.
 */
#include "mp4_remux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "mp4_remux";
#else
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
#endif

/* Cap stsz so a hostile/absurd sample count can't exhaust PSRAM. ~6.4h at
 * 44100 Hz (1024 samples/frame → 1M frames). */
#define MAX_SAMPLES      (1u * 1000u * 1000u)
#define MAX_SAMPLE_BYTES (64u * 1024u)   /* a single AAC frame is < 8KB */
#define MAX_STSC         64               /* stsc entries (podcasts have ~2-5) */

/* Sparse seek index: one entry per second.  4h at 1 entry/s = 14400 entries
 * × 8 bytes = ~115 KB (PSRAM).  Beyond this we stop recording and the player's
 * linear-estimate fallback covers the tail. */
#define MAX_INDEX_ENTRIES (4u * 3600u)

/* Side-car seek-index magic ("NCID" little-endian).  MUST match the reader in
 * system/audio_player/audio_player.c. */
#define SEEK_INDEX_MAGIC   0x4443494Eu
#define SEEK_INDEX_VERSION 1u

/* One index entry: absolute playback second → ADTS byte offset (frame-aligned). */
typedef struct { uint32_t sec; uint32_t byte; } seek_idx_ent_t;

/* ADTS sampling_frequency_index table. */
static const int k_sf_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350
};

static uint32_t be32(const unsigned char *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | ((uint32_t)b[3]);
}
static uint64_t be64(const unsigned char *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

static bool rd32(FILE *f, long off, uint32_t *out) {
    unsigned char b[4];
    if (fseek(f, off, SEEK_SET) != 0) return false;
    if (fread(b, 1, 4, f) != 4) return false;
    *out = be32(b);
    return true;
}

/* Read one box header at `off`, which must lie within [end]. Returns the header
 * size (8 or 16) on success, 0 on a malformed/truncated header. size==0 means
 * "runs to end of parent". */
static int box_read(FILE *f, long off, long end, uint64_t *size,
                    char type[4], long *payload, long *box_end) {
    unsigned char h[8];
    if (fseek(f, off, SEEK_SET) != 0) return 0;
    if (fread(h, 1, 8, f) != 8) return 0;
    uint64_t sz = be32(h);
    int hdr = 8;
    if (sz == 1) {
        unsigned char e[8];
        if (fread(e, 1, 8, f) != 8) return 0;
        sz = be64(e);
        hdr = 16;
    } else if (sz == 0) {
        sz = (uint64_t)(end - off);
    }
    if (sz < (uint64_t)hdr) return 0;
    if ((uint64_t)off + sz > (uint64_t)end) return 0;
    type[0] = (char)h[4]; type[1] = (char)h[5];
    type[2] = (char)h[6]; type[3] = (char)h[7];
    *size = sz;
    *payload = off + hdr;
    *box_end = off + (long)sz;
    return hdr;
}

/* Find a direct child box of `type` within [start,end). */
static bool box_find(FILE *f, long start, long end, const char *want,
                     long *payload, long *box_end) {
    long off = start;
    while (off + 8 <= end) {
        uint64_t sz; char type[4]; long pl, be_;
        int hdr = box_read(f, off, end, &sz, type, &pl, &be_);
        if (!hdr) return false;
        if (memcmp(type, want, 4) == 0) { *payload = pl; *box_end = be_; return true; }
        off = be_;
    }
    return false;
}

/* Decode an MPEG-4 variable-length descriptor length (ISO/IEC 14496-1 §8.3.3):
 * 7 bits per byte, high bit set means "another byte follows".  QuickTime/ffmpeg
 * write small lengths padded to 4 bytes as 0x80 0x80 0x80 0xNN, which a
 * single-byte read mis-parses. */
static bool desc_len(const unsigned char *d, size_t n, size_t *p, size_t *out) {
    size_t len = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= n) return false;
        unsigned char b = d[(*p)++];
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = len; return true; }
    }
    return false;
}

/* Pull the AAC AudioSpecificConfig (DecoderSpecificInfo) out of an esds
 * descriptor payload (version/flags already skipped). */
static bool esds_get_asc(const unsigned char *d, size_t n,
                         unsigned char *asc, size_t *asc_len) {
    size_t p = 0, len;

    if (p >= n || d[p] != 0x03) return false;          /* ES_Descriptor */
    p++;
    if (!desc_len(d, n, &p, &len)) return false;
    size_t es_end = p + len;
    if (es_end > n) return false;

    if (p + 3 > es_end) return false;
    uint8_t flags = d[p + 2];                          /* ES_ID (2) + flags (1) */
    p += 3;

    /* Optional ES_Descriptor fields (streamDependence / URL / OCR). */
    if (flags & 0x80) {
        if (p + 2 > es_end) return false;
        p += 2;
    }
    if (flags & 0x40) {
        if (p + 1 > es_end) return false;
        size_t ul = d[p++];
        if (p + ul > es_end) return false;
        p += ul;
    }
    if (flags & 0x20) {
        if (p + 2 > es_end) return false;
        p += 2;
    }

    if (p >= es_end || d[p] != 0x04) return false;     /* DecoderConfigDescriptor */
    p++;
    if (!desc_len(d, es_end, &p, &len)) return false;
    size_t dcd_end = p + len;
    if (dcd_end > es_end) return false;

    /* OTI (1) + streamType (1) + bufferSizeDB (3) + maxBitrate (4) + avgBitrate (4). */
    if (p + 13 > dcd_end) return false;
    p += 13;

    if (p >= dcd_end || d[p] != 0x05) return false;    /* DecoderSpecificInfo */
    p++;
    if (!desc_len(d, dcd_end, &p, &len)) return false;
    if (len == 0 || len > 64 || p + len > dcd_end) return false;

    memcpy(asc, d + p, len);
    *asc_len = len;
    return true;
}

/* Read n bits (MSB-first) from `asc` at bit offset *bit. */
static uint32_t asc_bits(const unsigned char *asc, size_t len, size_t *bit, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = (*bit) >> 3;
        int b = 7 - (int)((*bit) & 7);
        v <<= 1;
        if (byte < len) v |= (asc[byte] >> b) & 1;
        (*bit)++;
    }
    return v;
}

static int nearest_sfi(uint32_t rate) {
    int best = 4;                       /* 44100 default */
    int best_d = 0x7fffffff;
    for (int i = 0; i < 13; i++) {
        int d = k_sf_rates[i] - (int)rate;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Map the AudioSpecificConfig to the three ADTS header fields. */
static bool asc_to_adts(const unsigned char *asc, size_t len,
                        int *out_profile, int *out_sfi, int *out_cc) {
    size_t bit = 0;
    uint32_t aot = asc_bits(asc, len, &bit, 5);
    if (aot == 31) aot = 32 + asc_bits(asc, len, &bit, 6);   /* escape */
    uint32_t sfi = asc_bits(asc, len, &bit, 4);
    if (sfi == 15) sfi = (uint32_t)nearest_sfi(asc_bits(asc, len, &bit, 24));
    uint32_t cc = asc_bits(asc, len, &bit, 4);
    if (cc == 0 || cc > 7 || sfi > 12) return false;         /* PCE-in-band / unknown */

    int profile;
    switch (aot) {
        case 1:  profile = 0; break;   /* AAC Main */
        case 3:  profile = 2; break;   /* AAC SSR */
        case 2:                        /* AAC LC */
        case 4:                        /* AAC LTP */
        case 5:                        /* HE-AAC (SBR) — core is LC */
        case 29:                       /* HE-AAC v2 (PS) */
        default: profile = 1; break;
    }
    *out_profile = profile; *out_sfi = (int)sfi; *out_cc = (int)cc;
    return true;
}

/* Build a 7-byte ADTS header (MPEG-4, no CRC, VBR buffer-fullness 0x7FF). */
static void adts_header(unsigned char h[7], int profile, int sfi, int cc,
                        uint32_t frame_len) {
    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = (unsigned char)(((profile & 3) << 6) | ((sfi & 0xF) << 2) | ((cc >> 2) & 1));
    h[3] = (unsigned char)(((cc & 3) << 6) | ((frame_len >> 11) & 3));
    h[4] = (unsigned char)((frame_len >> 3) & 0xFF);
    h[5] = (unsigned char)(((frame_len & 7) << 5) | 0x1F);
    h[6] = 0xFC;
}

bool mp4_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    unsigned char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fflush(out) != 0) ok = false;
    fclose(out);
    return ok;
}

bool mp4_remux_to_adts(const char *src, const char *dst, int *out_duration_sec) {
    if (out_duration_sec) *out_duration_sec = 0;
    FILE *f = fopen(src, "rb");
    if (!f) { ESP_LOGE(TAG, "open src failed: %s", src); return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long file_size = ftell(f);
    if (file_size < 8) { ESP_LOGE(TAG, "src too small (%ld): %s", file_size, src); fclose(f); return false; }
    ESP_LOGI(TAG, "remux: %s (%ld bytes)", src, file_size);

    /* ── Top level: find moov and mdat ── */
    long moov_payload = -1, moov_end = 0;
    long off = 0;
    while (off + 8 <= file_size) {
        uint64_t sz; char type[4]; long pl, be_;
        if (!box_read(f, off, file_size, &sz, type, &pl, &be_)) break;
        if (memcmp(type, "moov", 4) == 0) { moov_payload = pl; moov_end = be_; }
        off = be_;
    }
    if (moov_payload < 0) { ESP_LOGW(TAG, "remux fail: no moov — %s", src); fclose(f); return false; }

    /* ── Descend moov → trak (audio) → mdia → minf → stbl ── */
    long trak_payload = -1, trak_end = 0;
    off = moov_payload;
    while (off + 8 <= moov_end) {
        uint64_t sz; char type[4]; long pl, be_;
        if (!box_read(f, off, moov_end, &sz, type, &pl, &be_)) break;
        if (memcmp(type, "trak", 4) == 0) {
            /* Pick the first audio trak: its stsd entry is mp4a. */
            long stbl = -1, stbl_end = 0, stsd = -1, stsd_end = 0;
            long mdia = -1, mdia_end = 0, minf = -1, minf_end = 0;
            if (box_find(f, pl, be_, "mdia", &mdia, &mdia_end) &&
                box_find(f, mdia, mdia_end, "minf", &minf, &minf_end) &&
                box_find(f, minf, minf_end, "stbl", &stbl, &stbl_end) &&
                box_find(f, stbl, stbl_end, "stsd", &stsd, &stsd_end)) {
                uint32_t ec = 0;
                if (rd32(f, stsd + 4, &ec) && ec > 0) {
                    long entry = stsd + 8;
                    uint64_t esz; char etype[4]; long epl, ebe;
                    if (box_read(f, entry, stsd_end, &esz, etype, &epl, &ebe) &&
                        memcmp(etype, "mp4a", 4) == 0) {
                        trak_payload = pl; trak_end = be_;
                        break;      /* found the audio track */
                    }
                }
            }
        }
        off = be_;
    }
    if (trak_payload < 0) { ESP_LOGW(TAG, "remux fail: no audio trak (mp4a) — %s", src); fclose(f); return false; }

    long mdia, mdia_end, minf, minf_end, stbl, stbl_end;
    if (!box_find(f, trak_payload, trak_end, "mdia", &mdia, &mdia_end) ||
        !box_find(f, mdia, mdia_end, "minf", &minf, &minf_end) ||
        !box_find(f, minf, minf_end, "stbl", &stbl, &stbl_end)) {
        ESP_LOGW(TAG, "remux fail: no stbl — %s", src);
        fclose(f); return false;
    }

    long stsd, stsd_end, stsc, stsc_end, stsz, stsz_end, stco, stco_end;
    if (!box_find(f, stbl, stbl_end, "stsd", &stsd, &stsd_end) ||
        !box_find(f, stbl, stbl_end, "stsc", &stsc, &stsc_end) ||
        !box_find(f, stbl, stbl_end, "stsz", &stsz, &stsz_end) ||
        !box_find(f, stbl, stbl_end, "stco", &stco, &stco_end)) {
        ESP_LOGW(TAG, "remux fail: stsd/stsc/stsz/stco missing — %s", src);
        fclose(f); return false;
    }

    /* ── stsd → mp4a → esds → ASC ── */
    uint32_t ec = 0;
    long entry = stsd + 8;
    if (!rd32(f, stsd + 4, &ec) || ec == 0) { fclose(f); return false; }
    uint64_t esz; char etype[4]; long mp4a_pl, mp4a_end;
    if (!box_read(f, entry, stsd_end, &esz, etype, &mp4a_pl, &mp4a_end) ||
        memcmp(etype, "mp4a", 4) != 0) { fclose(f); return false; }

    /* AudioSampleEntry fixed header is 28 bytes (ISO BMFF); a version-1
     * QuickTime-style entry carries 16 extra bytes before the child boxes, so
     * esds sits at +44 instead of +28.  Probe both. */
    long child = mp4a_pl + 28;
    long esds_pl = -1, esds_end = 0;
    if (!box_find(f, child, mp4a_end, "esds", &esds_pl, &esds_end) &&
        !box_find(f, child + 16, mp4a_end, "esds", &esds_pl, &esds_end)) {
        ESP_LOGW(TAG, "remux fail: no esds box in mp4a — %s", src);
        fclose(f); return false;
    }
    unsigned char *esds = (unsigned char *)malloc((size_t)(esds_end - esds_pl));
    if (!esds) { ESP_LOGE(TAG, "remux fail: esds alloc — %s", src); fclose(f); return false; }
    if (fseek(f, esds_pl + 4, SEEK_SET) != 0 ||      /* skip version/flags */
        fread(esds, 1, (size_t)(esds_end - esds_pl - 4), f) != (size_t)(esds_end - esds_pl - 4)) {
        free(esds); fclose(f); return false;
    }
    unsigned char asc[64]; size_t asc_len = 0;
    bool have_asc = esds_get_asc(esds, (size_t)(esds_end - esds_pl - 4), asc, &asc_len);
    free(esds);
    if (!have_asc || asc_len == 0) {
        ESP_LOGW(TAG, "remux fail: no ASC in esds — %s", src);
        fclose(f); return false;
    }

    int profile, sfi, cc;
    if (!asc_to_adts(asc, asc_len, &profile, &sfi, &cc)) {
        ESP_LOGW(TAG, "unsupported ASC (first byte 0x%02x)", asc[0]);
        fclose(f); return false;
    }
    int rate = k_sf_rates[sfi];

    /* ── stts + mdhd: authoritative per-frame sample count and timescale ──
     * The ASC audioObjectType only flags HE-AAC when SBR is signaled explicitly;
     * an implicitly-signaled SBR stream still declares AOT=2 (LC) in esds, which
     * would make every frame look like 1024 samples instead of 2048 and halve
     * the duration + seek-index time axis.  stts records the true samples-per-
     * frame straight from the muxer, so read it (defaulting to 1024 = AAC-LC). */
    uint32_t samples_per_frame = 1024;
    uint32_t timescale = (uint32_t)rate;
    {
        long stts, stts_end;
        if (box_find(f, stbl, stbl_end, "stts", &stts, &stts_end)) {
            uint32_t sc = 0;
            if (rd32(f, stts + 4, &sc) && sc >= 1) {
                uint32_t delta = 0;
                if (rd32(f, stts + 12, &delta) && delta >= 1 && delta <= 4096)
                    samples_per_frame = delta;
            }
        }
    }
    {
        long mdhd, mdhd_end;
        if (box_find(f, mdia, mdia_end, "mdhd", &mdhd, &mdhd_end)) {
            unsigned char ver = 0;
            if (fseek(f, mdhd, SEEK_SET) == 0 && fread(&ver, 1, 1, f) == 1) {
                long ts_off = (ver == 1) ? mdhd + 20 : mdhd + 12;
                uint32_t ts = 0;
                if (rd32(f, ts_off, &ts) && ts > 0) timescale = ts;
            }
        }
    }

    /* ── stsz: sample sizes (or a fixed size) ── */
    uint32_t sample_size = 0, sample_count = 0;
    if (!rd32(f, stsz + 4, &sample_size) || !rd32(f, stsz + 8, &sample_count)) {
        fclose(f); return false;
    }
    if (sample_count == 0 || sample_count > MAX_SAMPLES) { fclose(f); return false; }
    uint32_t *sizes = NULL;
    if (sample_size == 0) {
        sizes = (uint32_t *)malloc((size_t)sample_count * sizeof(uint32_t));
        if (!sizes) { fclose(f); return false; }
        unsigned char *raw = (unsigned char *)sizes;
        if (fseek(f, stsz + 12, SEEK_SET) != 0 ||
            fread(raw, 4, sample_count, f) != sample_count) {
            free(sizes); fclose(f); return false;
        }
        for (uint32_t i = 0; i < sample_count; i++) sizes[i] = be32(raw + i * 4);
    }

    /* ── stco: chunk offsets ── */
    uint32_t chunk_count = 0;
    if (!rd32(f, stco + 4, &chunk_count)) { free(sizes); fclose(f); return false; }
    if (chunk_count == 0) { free(sizes); fclose(f); return false; }
    uint32_t *chunks = (uint32_t *)malloc((size_t)chunk_count * sizeof(uint32_t));
    if (!chunks) { free(sizes); fclose(f); return false; }
    {
        unsigned char *raw = (unsigned char *)chunks;
        if (fseek(f, stco + 8, SEEK_SET) != 0 ||
            fread(raw, 4, chunk_count, f) != chunk_count) {
            free(chunks); free(sizes); fclose(f); return false;
        }
        for (uint32_t i = 0; i < chunk_count; i++) chunks[i] = be32(raw + i * 4);
    }

    /* ── stsc: chunk → samples-per-chunk (few entries) ── */
    uint32_t sc_count = 0;
    if (!rd32(f, stsc + 4, &sc_count) || sc_count == 0 || sc_count > MAX_STSC) {
        free(chunks); free(sizes); fclose(f); return false;
    }
    struct { uint32_t first_chunk, spc; } sc_ent[MAX_STSC];
    for (uint32_t i = 0; i < sc_count; i++) {
        uint32_t fc, spc, sdi;
        if (!rd32(f, stsc + 8 + (long)i * 12, &fc) ||
            !rd32(f, stsc + 12 + (long)i * 12, &spc) ||
            !rd32(f, stsc + 16 + (long)i * 12, &sdi)) {
            free(chunks); free(sizes); fclose(f); return false;
        }
        sc_ent[i].first_chunk = fc;
        sc_ent[i].spc = spc;
    }

    /* ── Stream samples → ADTS ── */
    unsigned char *samp = (unsigned char *)malloc(MAX_SAMPLE_BYTES);
    if (!samp) { free(chunks); free(sizes); fclose(f); return false; }

    FILE *out = fopen(dst, "wb");
    if (!out) { free(samp); free(chunks); free(sizes); fclose(f); return false; }

    /* Real duration + a sparse (time→byte) seek index, both built while the
     * samples stream out — no extra seeks or reads. */
    uint32_t duration_sec = (uint32_t)((int64_t)sample_count * samples_per_frame / timescale);
    uint32_t idx_cap = duration_sec + 2;
    if (idx_cap > MAX_INDEX_ENTRIES) idx_cap = MAX_INDEX_ENTRIES;
    seek_idx_ent_t *idx = (seek_idx_ent_t *)malloc((size_t)idx_cap * sizeof(seek_idx_ent_t));
    uint32_t idx_count = 0;
    int32_t  last_sec  = -1;
    uint64_t out_pos   = 0;   /* cumulative ADTS byte offset of the next frame */

    uint32_t si = 0;
    bool ok = true;
    for (uint32_t ci = 0; ci < chunk_count && ok; ci++) {
        /* samples-per-chunk for this chunk (1-based) = last stsc entry with
         * first_chunk <= chunk number. */
        uint32_t cn = ci + 1, spc = 1;
        for (uint32_t j = 0; j < sc_count; j++)
            if (sc_ent[j].first_chunk <= cn) spc = sc_ent[j].spc;
        long data_off = (long)chunks[ci];
        if (fseek(f, data_off, SEEK_SET) != 0) { ok = false; break; }
        for (uint32_t k = 0; k < spc; k++) {
            if (si >= sample_count) { ok = false; break; }
            uint32_t sz = (sample_size != 0) ? sample_size : sizes[si];
            if (sz == 0 || sz > MAX_SAMPLE_BYTES || sz + 7 > 8191) { ok = false; break; }
            if (fread(samp, 1, sz, f) != sz) { ok = false; break; }

            /* Record a 1s-granularity index entry at each whole-second boundary.
             * out_pos is the frame-aligned start of THIS frame, so an entry's
             * byte offset needs no further ADTS re-alignment at seek time. */
            if (idx && idx_count < idx_cap) {
                int32_t sec = (int32_t)((int64_t)si * samples_per_frame / timescale);
                if (sec != last_sec) {
                    idx[idx_count].sec  = (uint32_t)sec;
                    idx[idx_count].byte = (uint32_t)out_pos;
                    idx_count++;
                    last_sec = sec;
                }
            }

            unsigned char h[7];
            adts_header(h, profile, sfi, cc, sz + 7);
            if (fwrite(h, 1, 7, out) != 7 ||
                fwrite(samp, 1, sz, out) != sz) { ok = false; break; }
            out_pos += 7 + sz;
            si++;
        }
    }
    if (ok && si != sample_count) ok = false;

    free(samp); free(chunks); free(sizes);
    fclose(f);
    if (fflush(out) != 0) ok = false;
    fclose(out);

    if (!ok) {
        free(idx);
        ESP_LOGE(TAG, "remux failed (%" PRIu32 "/%" PRIu32 " samples)", si, sample_count);
        remove(dst);
        return false;
    }

    /* Persist the seek index as a side-car <dst>.idx.  Failure here is non-fatal:
     * the .aac is still playable, the player falls back to a linear estimate. */
    if (idx && idx_count > 0) {
        char idx_path[512 + 8];
        snprintf(idx_path, sizeof(idx_path), "%s.idx", dst);
        FILE *ifp = fopen(idx_path, "wb");
        if (ifp) {
            uint32_t magic = SEEK_INDEX_MAGIC;
            uint32_t ver   = SEEK_INDEX_VERSION;
            uint32_t fms   = (uint32_t)((int64_t)samples_per_frame * 1000 / timescale);
            uint32_t cnt   = idx_count;
            bool wok = (fwrite(&magic, 4, 1, ifp) == 1 &&
                        fwrite(&ver, 4, 1, ifp) == 1 &&
                        fwrite(&fms, 4, 1, ifp) == 1 &&
                        fwrite(&cnt, 4, 1, ifp) == 1);
            for (uint32_t i = 0; wok && i < idx_count; i++) {
                wok = (fwrite(&idx[i].sec, 4, 1, ifp) == 1 &&
                       fwrite(&idx[i].byte, 4, 1, ifp) == 1);
            }
            if (fflush(ifp) != 0) wok = false;
            fclose(ifp);
            if (!wok) remove(idx_path);
            ESP_LOGI(TAG, "seek index: %" PRIu32 " entries → %s (%s)",
                     idx_count, idx_path, wok ? "ok" : "failed");
        }
    }
    free(idx);

    if (out_duration_sec)
        *out_duration_sec = (int)duration_sec;
    ESP_LOGI(TAG, "remux ok: %" PRIu32 " samples, %d Hz, %d ch → %s",
             sample_count, rate, cc, dst);
    return true;
}

bool aac_file_probe(const char *path, int *out_duration_sec) {
    if (out_duration_sec) *out_duration_sec = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char buf[8192];        /* > max ADTS frame (8191 bytes) */
    size_t start = 0, avail = 0;    /* [start, start+avail) = unprocessed window */
    uint32_t frames = 0;
    int sfi = -1;
    bool ok = true;

    for (;;) {
        /* Keep at least one full header (7 bytes) in the window. */
        while (avail < 7) {
            if (start > 0) { memmove(buf, buf + start, avail); start = 0; }
            size_t r = fread(buf + avail, 1, sizeof(buf) - avail, f);
            if (r == 0) goto done;          /* EOF: whatever remains is tail */
            avail += r;
        }
        unsigned char *b = buf + start;
        if (b[0] != 0xFF || (b[1] & 0xF6) != 0xF0) { ok = false; break; }
        uint32_t fl = ((uint32_t)(b[3] & 3) << 11) | ((uint32_t)b[4] << 3) |
                      ((uint32_t)(b[5] >> 5) & 7);
        if (fl < 7) { ok = false; break; }
        if (sfi < 0) sfi = (b[2] >> 2) & 0xF;

        /* Pull the whole frame into the window (may span the boundary). */
        while (avail < fl) {
            if (start > 0) { memmove(buf, buf + start, avail); start = 0; }
            size_t r = fread(buf + avail, 1, sizeof(buf) - avail, f);
            if (r == 0) break;
            avail += r;
        }
        if (avail < fl) { ok = false; break; }   /* truncated frame */
        start += fl;
        avail -= fl;
        frames++;
    }

done:
    fclose(f);
    if (!ok || frames == 0 || sfi < 0 || sfi > 12) return false;
    if (avail != 0) return false;                /* trailing partial frame */
    if (out_duration_sec)
        *out_duration_sec = (int)((int64_t)frames * 1024 / k_sf_rates[sfi]);
    return true;
}
