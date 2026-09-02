/*
 * NomadCast — SD Card → USB Mass Storage Device (MSC) Test
 *
 * Exposes the SD card (SDMMC 1-bit) as a USB Mass Storage device, so a PC
 * sees it as a removable disk (USB flash-drive style).
 *
 * Pins (Leisound V1.1):
 *   EN_POWER = GPIO46   board power
 *   SD CLK   = GPIO10
 *   SD CMD   = GPIO11
 *   SD D0    = GPIO9
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "t_usb_msc";

#define PIN_EN_POWER    GPIO_NUM_46
#define PIN_SD_CLK      GPIO_NUM_10
#define PIN_SD_CMD      GPIO_NUM_11
#define PIN_SD_D0       GPIO_NUM_9

/* ---- TinyUSB MSC descriptors ---- */
#define EPNUM_MSC           1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum { EDPT_CTRL_OUT = 0x00, EDPT_CTRL_IN = 0x80, EDPT_MSC_OUT = 0x01, EDPT_MSC_IN = 0x81 };

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t msc_fs_configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static const char *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },   // 0: English
    "NomadCast",                     // 1: Manufacturer
    "SD Card MSC",                   // 2: Product
    "123456",                        // 3: Serial
    "SD Card",                       // 4: MSC
};

static tinyusb_msc_storage_handle_t s_storage = NULL;

/* ---- SD card init (SDMMC 1-bit) ---- */
static esp_err_t sdmmc_init(sdmmc_card_t **out_card)
{
    ESP_LOGI(TAG, "Initializing SD card (SDMMC 1-bit)...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk   = PIN_SD_CLK;
    slot_config.cmd   = PIN_SD_CMD;
    slot_config.d0    = PIN_SD_D0;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (!card) return ESP_ERR_NO_MEM;

    esp_err_t ret = (*host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        free(card);
        return ret;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        free(card);
        return ret;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC card init failed: %s (no card? check pins/power)", esp_err_to_name(ret));
        free(card);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    *out_card = card;
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_usb_msc: SD Card as USB Mass Storage ===");

    /* Board power on */
    gpio_set_direction(PIN_EN_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Init SD card */
    sdmmc_card_t *card = NULL;
    esp_err_t ret = sdmmc_init(&card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD init failed — insert a card and reboot");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* Create MSC storage exposed to the USB host (PC) */
    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    storage_cfg.medium.card = card;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage));

    /* Install TinyUSB with the MSC descriptors */
    ESP_LOGI(TAG, "Installing TinyUSB MSC...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "USB MSC ready — plug USB into PC; SD card should appear as a disk");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "STATUS: USB MSC running");
    }
}
