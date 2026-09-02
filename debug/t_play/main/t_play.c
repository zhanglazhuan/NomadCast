/*
 * NomadCast — M4A Audio Player (ESP-ADF Pipeline)
 *
 * Board: Leisound V1 (ESP32-S3)
 * Pipeline: data_source → aac_decoder → i2s_stream
 *
 * Input sources (compile-time switch via PLAY_SOURCE):
 *   PLAY_SOURCE_SD    — SD card FATFS (test.m4a in root)
 *   PLAY_SOURCE_HTTP  — HTTP network stream
 *
 * Usage:
 *   SD:   Put test.m4a on SD card root, flash and run
 *   HTTP: Set server URL below, ensure Wi-Fi is connected first
 */

#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "aac_decoder.h"
#include "board_leisound.h"

/* ---- Choose input source ---- */
#define PLAY_SOURCE_SD    1
#define PLAY_SOURCE_HTTP  2
#define PLAY_SOURCE       PLAY_SOURCE_SD

#if PLAY_SOURCE == PLAY_SOURCE_HTTP
#include "http_stream.h"
#endif

static const char *TAG = "t_play";

#if PLAY_SOURCE == PLAY_SOURCE_SD
    #define AUDIO_URI  BOARD_SD_MOUNT_POINT "/test.m4a"
#elif PLAY_SOURCE == PLAY_SOURCE_HTTP
    #define AUDIO_URI  "http://192.168.1.100/test.m4a"
#endif

/* ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "\n========== M4A Player (ADF Pipeline) ==========");

    /* ---- 1. NVS (required by ADF internals) ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- 2. Board hardware init ---- */
    ESP_ERROR_CHECK(board_power_init());

    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(board_i2c_init(&i2c_bus));

    es8156_handle_t es8156 = NULL;
    ESP_ERROR_CHECK(board_es8156_init(i2c_bus, &es8156));

    /* ---- 3. Input source (compile-time switch) ---- */
    audio_element_handle_t reader = NULL;

#if PLAY_SOURCE == PLAY_SOURCE_SD
    ESP_LOGI(TAG, "[3] SD card source");
    ESP_ERROR_CHECK(board_sd_init());

    struct stat st;
    if (stat(AUDIO_URI, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", AUDIO_URI);
        ESP_LOGE(TAG, "Put test.m4a on SD card root!");
        goto fail;
    }
    ESP_LOGI(TAG, "Found: %s (%ld bytes)", AUDIO_URI, (long)st.st_size);

    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    reader = fatfs_stream_init(&fatfs_cfg);

#elif PLAY_SOURCE == PLAY_SOURCE_HTTP
    ESP_LOGI(TAG, "[3] HTTP source: %s", AUDIO_URI);
    ESP_LOGI(TAG, "NOTE: Wi-Fi must be initialized before this point");

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    reader = http_stream_init(&http_cfg);
#endif

    audio_element_set_uri(reader, AUDIO_URI);

    /* ---- 4. Build audio pipeline ---- */
    ESP_LOGI(TAG, "[4] Pipeline: data_source → aac_decoder → i2s_stream");

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline) { ESP_LOGE(TAG, "Pipeline init fail"); goto fail; }

    /* 4a. aac_decoder (M4A → PCM) */
    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    audio_element_handle_t decoder = aac_decoder_init(&aac_cfg);

    /* 4b. i2s_stream (PCM → I2S) */
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = BOARD_PIN_I2S_BCLK;
    i2s_cfg.std_cfg.gpio_cfg.ws   = BOARD_PIN_I2S_LRCLK;
    i2s_cfg.std_cfg.gpio_cfg.dout = BOARD_PIN_I2S_DOUT;
    i2s_cfg.std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    audio_element_handle_t i2s_writer = i2s_stream_init(&i2s_cfg);

    /* 4c. Link: file → dec → i2s */
    audio_pipeline_register(pipeline, reader,  "file");
    audio_pipeline_register(pipeline, decoder, "dec");
    audio_pipeline_register(pipeline, i2s_writer, "i2s");
    const char *link_tag[3] = {"file", "dec", "i2s"};
    audio_pipeline_link(pipeline, link_tag, 3);

    /* ---- 5. Event listener ---- */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);

    /* ---- 6. Start playback ---- */
    ESP_LOGI(TAG, "[6] Starting playback...");
    audio_pipeline_run(pipeline);

    /* ---- 7. Event loop ---- */
    while (1) {
        audio_event_iface_msg_t msg;
        ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Event interface error: %d", ret);
            continue;
        }

        /* Decoder parsed audio header — adjust I2S clock */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *)decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(decoder, &music_info);
            ESP_LOGI(TAG, "Music info: %d Hz, %d bit, %d ch",
                     music_info.sample_rates, music_info.bits,
                     music_info.channels);
            audio_element_setinfo(i2s_writer, &music_info);
            i2s_stream_set_clk(i2s_writer, music_info.sample_rates,
                               music_info.bits, music_info.channels);
            continue;
        }

        /* Playback finished or stopped */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *)i2s_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int status = (int)msg.data;
            if (status == AEL_STATUS_STATE_STOPPED) {
                ESP_LOGI(TAG, "Playback finished");
                break;
            }
            if (status >= AEL_STATUS_ERROR_OPEN
                && status <= AEL_STATUS_ERROR_UNKNOWN) {
                ESP_LOGE(TAG, "Playback error: status=%d", status);
                break;
            }
        }
    }

    /* ---- 8. Cleanup ---- */
    ESP_LOGI(TAG, "[8] Cleanup...");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, reader);
    audio_pipeline_unregister(pipeline, decoder);
    audio_pipeline_unregister(pipeline, i2s_writer);
    audio_pipeline_remove_listener(pipeline);
    audio_event_iface_destroy(evt);
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(reader);
    audio_element_deinit(decoder);
    audio_element_deinit(i2s_writer);

    ESP_LOGI(TAG, "========== Done ==========");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "STATUS: PLAYBACK COMPLETE");
    }

fail:
    ESP_LOGE(TAG, "========== PLAYER FAILED ==========");
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
