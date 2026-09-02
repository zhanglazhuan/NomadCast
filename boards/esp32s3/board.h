/*
 * Leisound V1 — ADF-compatible board.h
 *
 * Single header providing everything ADF components (esp_peripherals,
 * audio_hal, etc.) expect from a board definition.  Most ADF peripherals
 * are disabled (FUNC_* = 0) because NomadCast uses its own drivers.
 *
 * Injected globally via include_directories() in the root CMakeLists.txt
 * so all ADF components can find it.  No vendor/esp-adf/ files are touched.
 *
 * All pin values reference leisound_v1.h — the single source of truth.
 */

#ifndef _AUDIO_BOARD_H_
#define _AUDIO_BOARD_H_

#include "audio_hal.h"
#include "board_pins_config.h"
#include "esp_peripherals.h"
#include "display_service.h"
#include "periph_sdcard.h"
#include "periph_lcd.h"

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "leisound_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Feature switches — all disabled because NomadCast owns its own drivers
 * ══════════════════════════════════════════════════════════════════════════ */

#define FUNC_LCD_SCREEN_EN          (0)
#define FUNC_SDCARD_EN              (0)
#define FUNC_LCD_TOUCH_EN           (0)
#define FUNC_AUDIO_CODEC_EN         (0)
#define FUNC_BUTTON_EN              (0)

/* ══════════════════════════════════════════════════════════════════════════
 * I2S helper
 * ══════════════════════════════════════════════════════════════════════════ */

#define I2S_GPIO_UNUSED             GPIO_NUM_NC

/* ══════════════════════════════════════════════════════════════════════════
 * Power amplifier (LEISOUND_PIN_AMP_EN from leisound_v1.h)
 * ══════════════════════════════════════════════════════════════════════════ */

#define BOARD_PA_GAIN               (0)
#define PA_ENABLE_GPIO              LEISOUND_PIN_AMP_EN
#define HEADPHONE_DETECT            (-1)

/* ══════════════════════════════════════════════════════════════════════════
 * Codec config (I2S slave, 48 kHz 16-bit)
 * ══════════════════════════════════════════════════════════════════════════ */

extern audio_hal_func_t AUDIO_CODEC_ES8156_DEFAULT_HANDLE;
extern audio_hal_func_t AUDIO_CODEC_ES8311_DEFAULT_HANDLE;
extern audio_hal_func_t AUDIO_CODEC_ES7210_DEFAULT_HANDLE;

#define AUDIO_CODEC_DEFAULT_CONFIG(){                       \
        .adc_input  = AUDIO_HAL_ADC_INPUT_LINE1,            \
        .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,             \
        .codec_mode = AUDIO_HAL_CODEC_MODE_BOTH,            \
        .i2s_iface = {                                      \
            .mode = AUDIO_HAL_MODE_SLAVE,                   \
            .fmt = AUDIO_HAL_I2S_NORMAL,                    \
            .samples = AUDIO_HAL_48K_SAMPLES,               \
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,            \
        },                                                  \
    };

#define AUDIO_ADC_INPUT_CH_FORMAT "RMNM"

/* ══════════════════════════════════════════════════════════════════════════
 * LCD — NomadCast drives ST7789 directly via esp_lcd (SPI2_HOST)
 *       Real: CS=10 DC=45 RST=8 SCK=12 MOSI=11
 * ══════════════════════════════════════════════════════════════════════════ */

#define LCD_CTRL_GPIO               GPIO_NUM_NC
#define LCD_RST_GPIO                GPIO_NUM_NC
#define LCD_DC_GPIO                 GPIO_NUM_NC
#define LCD_CS_GPIO                 GPIO_NUM_NC
#define LCD_CLK_GPIO                GPIO_NUM_NC
#define LCD_MOSI_GPIO               GPIO_NUM_NC
#define LCD_H_RES                   240
#define LCD_V_RES                   320
#define LCD_SWAP_XY                 (false)
#define LCD_MIRROR_X                (false)
#define LCD_MIRROR_Y                (false)
#define LCD_COLOR_INV               (false)
#define LCD_COLOR_SPACE             ESP_LCD_COLOR_SPACE_RGB

/* ══════════════════════════════════════════════════════════════════════════
 * SD card — NomadCast uses SDMMC 1-bit (CLK=1 CMD=14 D0=2), not SPI SD.
 *           ESP_SD_PIN_* are SPI-mode macros — NC because we use SDMMC.
 * ══════════════════════════════════════════════════════════════════════════ */

#define SDCARD_OPEN_FILE_NUM_MAX    5
#define SDCARD_INTR_GPIO            (-1)
#define SDCARD_PWR_CTRL             (-1)

#define ESP_SD_PIN_CLK              GPIO_NUM_NC
#define ESP_SD_PIN_CMD              GPIO_NUM_NC
#define ESP_SD_PIN_D0               GPIO_NUM_NC
#define ESP_SD_PIN_D1               GPIO_NUM_NC
#define ESP_SD_PIN_D2               GPIO_NUM_NC
#define ESP_SD_PIN_D3               GPIO_NUM_NC
#define ESP_SD_PIN_D4               GPIO_NUM_NC
#define ESP_SD_PIN_D5               GPIO_NUM_NC
#define ESP_SD_PIN_D6               GPIO_NUM_NC
#define ESP_SD_PIN_D7               GPIO_NUM_NC
#define ESP_SD_PIN_CD               GPIO_NUM_NC
#define ESP_SD_PIN_WP               GPIO_NUM_NC

/* ══════════════════════════════════════════════════════════════════════════
 * Touch — NomadCast uses its own GT911 driver
 * ══════════════════════════════════════════════════════════════════════════ */

#define TOUCH_PANEL_SWAP_XY         (0)
#define TOUCH_PANEL_INVERSE_X       (0)
#define TOUCH_PANEL_INVERSE_Y       (0)

/* ══════════════════════════════════════════════════════════════════════════
 * Button — NomadCast uses espressif/button
 * ══════════════════════════════════════════════════════════════════════════ */

#define INPUT_KEY_NUM               0

/* ══════════════════════════════════════════════════════════════════════════
 * Codec I2S ports
 * ══════════════════════════════════════════════════════════════════════════ */

#define CODEC_ADC_I2S_PORT          ((i2s_port_t)0)
#define CODEC_ADC_BITS_PER_SAMPLE   ((i2s_data_bit_width_t)32)
#define CODEC_ADC_SAMPLE_RATE       (48000)
#define RECORD_HARDWARE_AEC         (false)

/* ══════════════════════════════════════════════════════════════════════════
 * audio_board API — stubs for linking (never called by ADF components, only
 * by application code which NomadCast doesn't use)
 * ══════════════════════════════════════════════════════════════════════════ */

struct audio_board_handle {
    audio_hal_handle_t audio_hal;
    audio_hal_handle_t adc_hal;
};

typedef struct audio_board_handle *audio_board_handle_t;

audio_board_handle_t audio_board_init(void);
audio_hal_handle_t audio_board_codec_init(void);
audio_hal_handle_t audio_board_adc_init(void);
void *audio_board_lcd_init(esp_periph_set_handle_t set, void *cb);
display_service_handle_t audio_board_blue_led_init(void);
esp_err_t audio_board_key_init(esp_periph_set_handle_t set);
esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode);
audio_board_handle_t audio_board_get_handle(void);
esp_err_t audio_board_deinit(audio_board_handle_t audio_board);

#ifdef __cplusplus
}
#endif

#endif /* _AUDIO_BOARD_H_ */
