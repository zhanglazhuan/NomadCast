/*
 * NomadCast — Earphone Diagnostic (ES8156)
 *
 * 诊断功能:
 *   1. I2C 全扫描 (找所有设备)
 *   2. PCNT 检测 MCLK 是否输出了
 *   3. ES8156 寄存器 dump (播放前后对比)
 *
 * PCB V1.2: pins come from nomadcast.h.
 * ES8156 headphone DAC; HT6872 speaker amplifier remains disabled.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "es8156.h"
#include "nomadcast.h"
#include "esp_timer.h"

static const char *TAG = "diag";

#define PIN_EN_POWER NOMADCAST_PIN_AP_POWER
#define PIN_AMP_EN NOMADCAST_PIN_HP_DETECT
#define PIN_I2C_SDA NOMADCAST_PIN_I2C_SDA
#define PIN_I2C_SCL NOMADCAST_PIN_I2C_SCL
#define PIN_I2S_MCLK NOMADCAST_PIN_I2S_MCLK
#define PIN_I2S_BCLK NOMADCAST_PIN_I2S_BCLK
#define PIN_I2S_LRCLK NOMADCAST_PIN_I2S_WS
#define PIN_I2S_DOUT NOMADCAST_PIN_I2S_DOUT
#define PIN_AP_EN NOMADCAST_PIN_AMP_EN
#define ES8156_ADDR NOMADCAST_ES8156_ADDR
#define SAMPLE_RATE      44100

static i2c_master_bus_handle_t i2c_bus;
static es8156_handle_t         es8156;
static i2s_chan_handle_t       i2s_tx;

/* ---- I2C scan ---- */
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "=== I2C address probe (0x08-0x77; no register commands) ===");
    int found = 0;
    for (uint8_t addr = 8; addr < 0x78; ++addr) {
        if (i2c_master_probe(i2c_bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "  ACK at 0x%02X", addr);
            ++found;
        }
    }
    ESP_LOGI(TAG, "Total ACK addresses: %d", found);
    ESP_ERROR_CHECK(i2c_master_bus_reset(i2c_bus));
}

/* ---- MCLK detect via PCNT ---- */
static void check_mclk(void)
{
    ESP_LOGI(TAG, "=== MCLK Check (PCNT on IO%d) ===", PIN_I2S_MCLK);

    pcnt_unit_config_t unit_cfg = {
        .low_limit = -32768,
        .high_limit = 32767,
        .flags.accum_count = true,
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &pcnt_unit));

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = PIN_I2S_MCLK,
        .level_gpio_num = -1,
        .flags.io_loop_back = true,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_cfg, &pcnt_chan));

    /* 上升沿计数 */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(pcnt_unit, 32767));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(pcnt_unit, -32768));
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

    int64_t started = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(100));

    int count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &count));
    int64_t elapsed = esp_timer_get_time() - started;
    ESP_ERROR_CHECK(pcnt_unit_stop(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_disable(pcnt_unit));
    pcnt_del_channel(pcnt_chan);
    pcnt_del_unit(pcnt_unit);

    /* 11.2896MHz * 0.1s = ~1,128,960 pulses */
    if (count > 1000) {
        ESP_LOGI(TAG, "MCLK OK! PCNT=%d (~%.1f MHz)", count, (double)count / elapsed);
    } else {
        ESP_LOGE(TAG, "MCLK DEAD! PCNT=%d — no clock on IO%d", count, PIN_I2S_MCLK);
    }
}

/* ---- ES8156 full register dump ---- */
static void dump_all_regs(const char *label)
{
    ESP_LOGI(TAG, "=== ES8156 Registers %s ===", label);
    for (int addr = 0; addr <= 0x25; addr++) {
        uint8_t v = 0;
        if (es8156_read_reg(es8156, addr, &v) == ESP_OK) {
            if (v != 0) {
                ESP_LOGI(TAG, "  [0x%02X] = 0x%02X", addr, v);
            }
        }
    }
    /* Chip ID */
    uint16_t id;
    uint8_t version;
    ESP_ERROR_CHECK(es8156_read_chip_id(es8156, &id));
    ESP_ERROR_CHECK(es8156_read_reg(es8156, 0xFF, &version));
    ESP_LOGI(TAG, "  ChipID=0x%04X Ver=0x%02X", id, version);
}

/* ---- Main ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ES8156 Diagnostic / PCB V1.2 ===");
    ESP_ERROR_CHECK(gpio_set_direction(PIN_AP_EN, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(PIN_AP_EN, 0));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_EN_POWER, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(PIN_EN_POWER, 1));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_AMP_EN, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_AMP_EN, GPIO_FLOATING));
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "POWER=%d AP_EN(GPIO%d)=%d HP_DETECT(GPIO%d)=%d",
             gpio_get_level(PIN_EN_POWER), PIN_AP_EN, gpio_get_level(PIN_AP_EN),
             PIN_AMP_EN, gpio_get_level(PIN_AMP_EN));
    ESP_LOGI(TAG, "I2C SDA=%d SCL=%d; I2S MCLK=%d BCLK=%d LRCLK=%d DOUT=%d",
             PIN_I2C_SDA, PIN_I2C_SCL, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);

    /* I2C */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL, .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* 1. Scan I2C */
    i2c_scan();

    /* 2. Init ES8156 */
    es8156_config_t cfg = {.i2c_bus = i2c_bus, .i2c_address = ES8156_ADDR};
    if (es8156_initialize(&cfg, &es8156) != ESP_OK) { ESP_LOGE(TAG, "ES8156 init FAIL"); goto end; }

    uint16_t cid;
    ESP_ERROR_CHECK(es8156_read_chip_id(es8156, &cid));
    ESP_LOGI(TAG, "ES8156 Chip ID: 0x%04X", cid);

    /* Dump before configure */
    dump_all_regs("BEFORE config");

    ESP_ERROR_CHECK(es8156_configure(es8156));  // DACHPModeOn=1 headphone mode (main2 ref)

    dump_all_regs("AFTER config");

    /* 3. I2S master supplies MCLK at 256fs to ES8156. */
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &i2s_tx, NULL));
    i2s_std_config_t sc = {
        // 使用官方标准的时钟配置宏，它会自动处理好内部时钟源和分频关系！
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE), 
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,  // ESP32 supplies MCLK to ES8156
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };

    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));

    /* Write silence */
    int16_t sil[512] = {0};
    size_t bw;
    ESP_ERROR_CHECK(i2s_channel_write(i2s_tx, sil, sizeof(sil), &bw, 1000));
    ESP_ERROR_CHECK(bw == sizeof(sil) ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "I2S enabled, MCLK output on IO%d, silence preloaded", PIN_I2S_MCLK);

    /* 4. Check MCLK */
    check_mclk();

    /* 5. 持续 1kHz 蜂鸣 — 方便逻辑分析仪抓 I2S 波形 */
    {
        #define TONE_FREQ     800
        #define TONE_PERIOD   (SAMPLE_RATE / TONE_FREQ)  // 55 samples
        int16_t buf[TONE_PERIOD * 2];
        for (int i = 0; i < TONE_PERIOD; i++) {
            int16_t s = (int16_t)(10000.0 * sin(2.0 * M_PI * i / TONE_PERIOD));
            buf[i * 2] = buf[i * 2 + 1] = s;
        }
        ESP_LOGI(TAG, "=== Continuous %dHz beep — capture I2S now ===", TONE_FREQ);
        int loop = 0;
        while (1) {
            size_t bw;
            ESP_ERROR_CHECK(i2s_channel_write(i2s_tx, buf, sizeof(buf), &bw, 1000));
            ESP_ERROR_CHECK(bw == sizeof(buf) ? ESP_OK : ESP_FAIL);
            if (++loop % 4000 == 0) ESP_LOGI(TAG, "Beep loop %d", loop);
        }
    }

end:
    ESP_LOGE(TAG, "Init failed");
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
