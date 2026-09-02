/*
 * NomadCast — Earphone Diagnostic (ES8156)
 *
 * 诊断功能:
 *   1. I2C 全扫描 (找所有设备)
 *   2. PCNT 检测 MCLK 是否输出了
 *   3. ES8156 寄存器 dump (播放前后对比)
 *
 * === Leisound V1 Pins (耳机通道) ===
 *   EN_PWR   = GPIO46   全板外设电源
 *   AMP_EN   = GPIO43   耳机检测 (输入, 高有效)
 *   AP_EN    = GPIO21   喇叭功放使能 (耳机播放时拉低)
 *   I2C SDA  = GPIO47
 *   I2C SCL  = GPIO48
 *   I2S MCLK = GPIO4    (ESP32 输出 MCLK)
 *   I2S BCLK = GPIO5
 *   I2S LRCLK= GPIO6
 *   I2S DIN  = GPIO7
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

static const char *TAG = "diag";

#define PIN_EN_POWER     GPIO_NUM_46
#define PIN_AMP_EN       GPIO_NUM_18  // 耳机检测 (输入)
#define PIN_I2C_SDA      GPIO_NUM_38
#define PIN_I2C_SCL      GPIO_NUM_45
#define PIN_I2S_MCLK     GPIO_NUM_1
#define PIN_I2S_BCLK     GPIO_NUM_2
#define PIN_I2S_LRCLK    GPIO_NUM_41
#define PIN_I2S_DIN      GPIO_NUM_42
#define PIN_AP_EN        GPIO_NUM_44

#define ES8156_ADDR      0x08
#define SAMPLE_RATE      44100

static i2c_master_bus_handle_t i2c_bus;
static es8156_handle_t         es8156;
static i2s_chan_handle_t       i2s_tx;

/* ---- I2C scan ---- */
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "=== I2C Bus Scan (0x01-0x7F) ===");
    int found = 0;
    for (uint8_t addr = 1; addr < 128; addr++) {
        i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000};
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(i2c_bus, &d, &dev) == ESP_OK) {
            uint8_t dummy;
            if (i2c_master_receive(dev, &dummy, 1, 100) == ESP_OK) {
                /* 读 Chip ID (0xFD/0xFE) */
                uint8_t id[2] = {0};
                uint8_t reg = 0xFD;
                if (i2c_master_transmit_receive(dev, &reg, 1, id, 2, 100) == ESP_OK) {
                    ESP_LOGI(TAG, "  [0x%02X] ChipID=0x%02X%02X", addr, id[0], id[1]);
                } else {
                    ESP_LOGI(TAG, "  [0x%02X] (no chip ID read)", addr);
                }
                found++;
            }
            i2c_master_bus_rm_device(dev);
        }
    }
    ESP_LOGI(TAG, "Total devices: %d", found);
}

/* ---- MCLK detect via PCNT ---- */
static void check_mclk(void)
{
    ESP_LOGI(TAG, "=== MCLK Check (PCNT on IO%d) ===", PIN_I2S_MCLK);

    pcnt_unit_config_t unit_cfg = {
        .low_limit = -32768,
        .high_limit = 32767,
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &pcnt_unit));

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = PIN_I2S_MCLK,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_cfg, &pcnt_chan));

    /* 上升沿计数 */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

    vTaskDelay(pdMS_TO_TICKS(100));  // 等 100ms

    int count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &count));
    ESP_ERROR_CHECK(pcnt_unit_stop(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_disable(pcnt_unit));
    pcnt_del_channel(pcnt_chan);
    pcnt_del_unit(pcnt_unit);

    /* 11.2896MHz * 0.1s = ~1,128,960 pulses */
    if (count > 1000) {
        ESP_LOGI(TAG, "MCLK OK! PCNT=%d (~%.1f MHz)", count, count / 100000.0);
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
    uint8_t id[2] = {0};
    es8156_read_reg(es8156, 0xFD, &id[0]);
    es8156_read_reg(es8156, 0xFE, &id[1]);
    es8156_read_reg(es8156, 0xFF, &id[0]);  // reuse id[0] for version
    ESP_LOGI(TAG, "  ChipID=0x%02X%02X Ver=0x%02X", id[0], id[1], (int)id[0]);
}

/* ---- Main ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ES8156 Diagnostic ===");

    /* 功放使能 — 尝试 HIGH (耳机通路可能经过功放) */
    gpio_set_direction(PIN_AP_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_AP_EN, 0);  // 之前是 0, 试试 1

    /* Power */
    gpio_set_direction(PIN_EN_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 耳机使能 — 尝试主动输出 HIGH (可能是耳机通路开关)
    gpio_set_direction(PIN_AMP_EN, GPIO_MODE_INPUT);
    // gpio_set_level(PIN_AMP_EN, 1);
    ESP_LOGI(TAG, "AMP_EN(43) driven HIGH for headphone path");
    // ESP_LOGI(TAG, "HW: EN_POWER(46)=HIGH  AMP_EN(43)=%d  AP_EN(21)=LOW", hp_plugged);
    // if (!hp_plugged) {
    //     ESP_LOGW(TAG, "Headphone NOT detected on AMP_EN(43)!  Check wiring.");
    // }

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
    es8156_read_chip_id(es8156, &cid);
    ESP_LOGI(TAG, "ES8156 Chip ID: 0x%04X", cid);

    /* Dump before configure */
    dump_all_regs("BEFORE config");

    es8156_configure(es8156);  // DACHPModeOn=1 headphone mode (main2 ref)

    dump_all_regs("AFTER config");

    /* 3. I2S (外部 MCLK 输入, 256fs) */
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &i2s_tx, NULL));
    i2s_std_config_t sc = {
        // 使用官方标准的时钟配置宏，它会自动处理好内部时钟源和分频关系！
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE), 
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,  // GPIO4 — ESP32 输出 MCLK 给 ES8156
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DIN,
            .din = I2S_GPIO_UNUSED,
        },
    };

    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));

    /* Write silence */
    int16_t sil[512] = {0};
    size_t bw;
    i2s_channel_write(i2s_tx, sil, sizeof(sil), &bw, portMAX_DELAY);
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
            i2s_channel_write(i2s_tx, buf, sizeof(buf), &bw, portMAX_DELAY);
            if (++loop % 100 == 0) ESP_LOGI(TAG, "Beep loop %d", loop);
        }
    }

end:
    ESP_LOGE(TAG, "Init failed");
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
