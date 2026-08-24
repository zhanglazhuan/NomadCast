/*
 * NomadCast — SD Card Test (ESP-IDF Native SDMMC 1-bit Mode)
 *
 * 参考 Leisound V1 硬件针脚定义，使用 ESP-IDF 原生 sdmmc + VFS FAT
 * 对 SD 卡进行完整的读写验证。
 *
 * === Leisound V1 SD 卡针脚 ===
 *
 *   SD Pin | SD 模式  | GPIO
 *   -------|----------|-------
 *   5      | CLK      | GPIO1
 *   2      | CMD      | GPIO14
 *   7      | DAT0     | GPIO2
 *
 * === Leisound V1 其他相关针脚 ===
 *
 *   GPIO46  EN_POWER  全板 3.3V/5V 电源使能 (HIGH=ON)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"

static const char *TAG = "t_sd";

/* ========================================================================
 * Leisound V1 针脚定义
 * ======================================================================== */

#define PIN_EN_POWER   GPIO_NUM_46   // 全板外设电源使能 (HIGH = ON)

#define PIN_SD_CLK     GPIO_NUM_10    // SD CLK
#define PIN_SD_CMD     GPIO_NUM_11   // SD CMD
#define PIN_SD_DAT0    GPIO_NUM_9    // SD DAT0

/* ========================================================================
 * 测试参数
 * ======================================================================== */

#define MOUNT_POINT    "/sdcard"
#define TEST_FILE      MOUNT_POINT "/podcastkit_test.txt"
#define TEST_DIR       MOUNT_POINT "/podcastkit_test_dir"
#define TEST_MSG       "NomadCast SD Test — write OK"

/* ========================================================================
 * 工具函数
 * ======================================================================== */

static void list_dir(const char *path, int levels)
{
    ESP_LOGI(TAG, "  [%s]", path);

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGI(TAG, "    (cannot open)");
        return;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "    [DIR]  %s", entry->d_name);
            if (levels > 0) {
                list_dir(full_path, levels - 1);
            }
        } else {
            ESP_LOGI(TAG, "           %s  (%ld bytes)", entry->d_name, (long)st.st_size);
        }
        count++;
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "    (empty)");
    }
}

static bool write_file(const char *path, const char *message)
{
    ESP_LOGI(TAG, "  Writing: %s", path);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "    FAIL: cannot open for write");
        return false;
    }

    size_t len = strlen(message);
    size_t written = fwrite(message, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "    FAIL: wrote %zu / %zu bytes", written, len);
        return false;
    }

    ESP_LOGI(TAG, "    wrote %zu bytes OK", written);
    return true;
}

static bool read_and_verify(const char *path, const char *expected)
{
    ESP_LOGI(TAG, "  Verifying: %s", path);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "    FAIL: cannot open for read");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        ESP_LOGE(TAG, "    FAIL: malloc failed");
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(buf, 1, size, f);
    buf[read_bytes] = '\0';
    fclose(f);

    if (strcmp(buf, expected) != 0) {
        ESP_LOGE(TAG, "    FAIL: content mismatch");
        ESP_LOGE(TAG, "    expected: %s", expected);
        ESP_LOGE(TAG, "    got:      %s", buf);
        free(buf);
        return false;
    }

    free(buf);
    ESP_LOGI(TAG, "    verify OK (%ld bytes)", size);
    return true;
}

/* ========================================================================
 * 测试用例
 * ======================================================================== */

static bool test_power_init(void)
{
    ESP_LOGI(TAG, "--- Test 1: Power Init ---");

    gpio_set_direction(PIN_EN_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN_POWER, 1);
    ESP_LOGI(TAG, "  EN_POWER (GPIO46) → HIGH");

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "  PASS");
    return true;
}

static sdmmc_card_t *test_sdmmc_init(void)
{
    ESP_LOGI(TAG, "--- Test 2: SDMMC Init (1-bit Native Mode) ---");

    /*
     * ESP32-S3 的 USB OTG 和 SDMMC 共用 GDMA 通道。
     * 如果使用 USB CDC 作为控制台，需要在 SDMMC 初始化前释放 USB DMA，
     * 初始化完成后再恢复。
     *
     * 这里先 flush stdout/stderr，然后短暂暂停控制台输出。
     */
    fflush(stdout);
    fflush(stderr);
    vTaskDelay(pdMS_TO_TICKS(100));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_DAT0;
    slot_config.width = 1;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  FAIL: esp_vfs_fat_sdmmc_mount = %s (0x%x)",
                 esp_err_to_name(ret), ret);
        return NULL;
    }

    ESP_LOGI(TAG, "  SD card mounted at %s", MOUNT_POINT);
    ESP_LOGI(TAG, "  PASS");
    return card;
}

static void test_card_info(sdmmc_card_t *card)
{
    ESP_LOGI(TAG, "--- Test 3: Card Info ---");

    if (!card) {
        ESP_LOGE(TAG, "  FAIL: no card");
        return;
    }

    const char *type_str;
    switch (card->ocr & SD_OCR_SDHC_CAP) {
    case SD_OCR_SDHC_CAP:
        type_str = "SDHC/SDXC";
        break;
    default:
        type_str = "SDSC";
        break;
    }

    ESP_LOGI(TAG, "  Card Type: %s", type_str);
    ESP_LOGI(TAG, "  Card Size: %llu MB", (uint64_t)((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024));
    ESP_LOGI(TAG, "  Sector Size: %d bytes", card->csd.sector_size);
    ESP_LOGI(TAG, "  Max Frequency: %d kHz", card->max_freq_khz);
    ESP_LOGI(TAG, "  PASS");
}

/* ========================================================================
 * 主入口
 * ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  NomadCast — SD Card Test");
    ESP_LOGI(TAG, "  Native SDMMC 1-bit Mode");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Pin Configuration:");
    ESP_LOGI(TAG, "    CLK      = GPIO%d", PIN_SD_CLK);
    ESP_LOGI(TAG, "    CMD      = GPIO%d", PIN_SD_CMD);
    ESP_LOGI(TAG, "    DAT0     = GPIO%d", PIN_SD_DAT0);
    ESP_LOGI(TAG, "    EN_POWER = GPIO%d", PIN_EN_POWER);

    /* ====== 测试 ====== */

    // Test 1: 上电
    if (!test_power_init()) { goto fail; }

    // Test 2: SDMMC 初始化 + 挂载 FAT
    sdmmc_card_t *card = test_sdmmc_init();
    if (!card) { goto fail; }

    // Test 3: 卡信息
    test_card_info(card);

    // Test 4: 列出根目录
    ESP_LOGI(TAG, "--- Test 4: List Root ---");
    list_dir(MOUNT_POINT, 1);
    ESP_LOGI(TAG, "  PASS");

    // Test 5: 写文件
    ESP_LOGI(TAG, "--- Test 5: Write File ---");
    if (!write_file(TEST_FILE, TEST_MSG)) { goto fail; }
    ESP_LOGI(TAG, "  PASS");

    // Test 6: 读回校验
    ESP_LOGI(TAG, "--- Test 6: Read + Verify ---");
    if (!read_and_verify(TEST_FILE, TEST_MSG)) { goto fail; }
    ESP_LOGI(TAG, "  PASS");

    // Test 7: 创建目录
    ESP_LOGI(TAG, "--- Test 7: Create Directory ---");
    {
        struct stat st;
        if (stat(TEST_DIR, &st) == 0) {
            // 清理旧目录
            char sub_path[256];
            snprintf(sub_path, sizeof(sub_path), "%s/hello.txt", TEST_DIR);
            unlink(sub_path);
            rmdir(TEST_DIR);
        }

        if (mkdir(TEST_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "  FAIL: mkdir failed");
            goto fail;
        }

        char sub_path[256];
        snprintf(sub_path, sizeof(sub_path), "%s/hello.txt", TEST_DIR);
        if (!write_file(sub_path, "test")) { goto fail; }

        list_dir(TEST_DIR, 0);
        ESP_LOGI(TAG, "  PASS");
    }

    // Test 8: 追加写入
    ESP_LOGI(TAG, "--- Test 8: Append Write ---");
    {
        FILE *f = fopen(TEST_FILE, "a");
        if (!f) {
            ESP_LOGE(TAG, "  FAIL: cannot open for append");
            goto fail;
        }

        const char *append_msg = "\nSecond line appended.";
        size_t len = strlen(append_msg);
        size_t written = fwrite(append_msg, 1, len, f);
        fclose(f);

        if (written != len) {
            ESP_LOGE(TAG, "  FAIL: append wrote %zu / %zu bytes", written, len);
            goto fail;
        }

        ESP_LOGI(TAG, "  appended %zu bytes OK", written);

        // 读出追加后的内容
        char expected[256];
        snprintf(expected, sizeof(expected), "%s%s", TEST_MSG, append_msg);
        if (!read_and_verify(TEST_FILE, expected)) { goto fail; }

        ESP_LOGI(TAG, "  PASS");
    }

    // Test 9: 清理
    ESP_LOGI(TAG, "--- Test 9: Cleanup ---");
    {
        char sub_path[256];
        snprintf(sub_path, sizeof(sub_path), "%s/hello.txt", TEST_DIR);
        unlink(sub_path);
        rmdir(TEST_DIR);
        unlink(TEST_FILE);
        ESP_LOGI(TAG, "  removed all test files");
    }

    ESP_LOGI(TAG, "\n--- Final Root ---");
    list_dir(MOUNT_POINT, 1);
    ESP_LOGI(TAG, "  DONE");

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ALL SD CARD TESTS PASSED");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "STATUS: SD TEST PASSED");
    }

fail:
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "  SD CARD TEST FAILED");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGE(TAG, "STATUS: SD TEST FAILED");
    }
}
