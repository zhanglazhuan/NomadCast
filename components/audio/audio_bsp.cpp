/*
 * Audio BSP — I2S Audio Codec Abstraction
 *
 * Wraps the ESP Codec Device API (esp_codec_dev) for playback and recording.
 * Board-specific I2S/I2C pin configuration is read from codec_board's
 * board_cfg.txt at runtime.
 *
 * Leisound V1 defaults to "Leisound_V1" board entry:
 *   - DUMMY codec (I2S-only, no I2C control — matches HT6872 direct amp)
 *   - I2C: SDA=GPIO47, SCL=GPIO48 (for future ES8156 support)
 *   - I2S: BCLK=39, WS=41, DOUT=40, DIN=42, MCLK=-1 (no MCLK)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "audio_bsp.h"
#include "esp_log.h"

static const char *TAG = "Audio";

I2sAudioCodec::I2sAudioCodec(const char *board_name)
{
    set_codec_board_type(board_name);

    codec_init_cfg_t codec_cfg = {};
    codec_cfg.in_mode    = CODEC_I2S_MODE_STD;
    codec_cfg.out_mode   = CODEC_I2S_MODE_STD;
    codec_cfg.in_use_tdm = false;
    codec_cfg.reuse_dev  = false;

    ESP_ERROR_CHECK(init_codec(&codec_cfg));

    playback_ = get_playback_handle();
    record_   = get_record_handle();

    ESP_LOGI(TAG, "Initialized (board=%s)", board_name);
}

I2sAudioCodec::~I2sAudioCodec()
{
}

void I2sAudioCodec::I2sAudio_SetCodecInfo(const char *codec_name,
                                            int open_en,
                                            int sample_rate,
                                            int channel,
                                            int bits_per_sample)
{
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = sample_rate;
    fs.channel         = channel;
    fs.bits_per_sample = bits_per_sample;

    if (open_en) {
        if (!strcmp(codec_name, "es8311")) {
            esp_codec_dev_open(playback_, &fs);
        } else if (!strcmp(codec_name, "es7210")) {
            esp_codec_dev_open(record_, &fs);
        } else {
            /* "es8311 & es7210" or any other combined name —
             * open both playback and record */
            esp_codec_dev_open(playback_, &fs);
            esp_codec_dev_open(record_, &fs);
        }
    }
}

void I2sAudioCodec::I2sAudio_SetSpeakerVol(int vol)
{
    esp_codec_dev_set_out_vol(playback_, vol);
}

void I2sAudioCodec::I2sAudio_SetMicGain(float db_value)
{
    esp_codec_dev_set_in_gain(record_, db_value);
}

void I2sAudioCodec::I2sAudio_CloseSpeaker(void)
{
    esp_codec_dev_close(playback_);
}

void I2sAudioCodec::I2sAudio_CloseMic(void)
{
    esp_codec_dev_close(record_);
}

int I2sAudioCodec::I2sAudio_PlayWrite(void *ptr, int ptr_len)
{
    return esp_codec_dev_write(playback_, ptr, ptr_len);
}

int I2sAudioCodec::I2sAudio_EchoRead(void *ptr, int ptr_len)
{
    return esp_codec_dev_read(record_, ptr, ptr_len);
}
