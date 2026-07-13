#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H

#include "codec_board.h"
#include "codec_init.h"
#include "esp_codec_dev.h"

class I2sAudioCodec
{
private:
    esp_codec_dev_handle_t playback_;
    esp_codec_dev_handle_t record_;

public:
    I2sAudioCodec(const char *board_name = "Leisound_V1");
    ~I2sAudioCodec();

    void I2sAudio_SetSpeakerVol(int vol);
    void I2sAudio_SetMicGain(float db_value);

    void I2sAudio_SetCodecInfo(const char *codec_name, int open_en,
                                int sample_rate, int channel, int bits_per_sample);
    void I2sAudio_CloseSpeaker(void);
    void I2sAudio_CloseMic(void);
    int  I2sAudio_PlayWrite(void *ptr, int ptr_len);
    int  I2sAudio_EchoRead(void *ptr, int ptr_len);
};

#endif
