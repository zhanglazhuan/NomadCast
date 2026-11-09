#include "usb_storage.h"
#include "sd_access_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include "nomadcast.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#define PIN_SD_CLK NOMADCAST_PIN_SD_CLK
#define PIN_SD_CMD NOMADCAST_PIN_SD_CMD
#define PIN_SD_D0 NOMADCAST_PIN_SD_DAT0
#define SD_PATH "/sdcard"
static const char *TAG = "usb_storage";
typedef enum { STORAGE_LOCAL, STORAGE_ENTERING, STORAGE_USB, STORAGE_LEAVING, STORAGE_ERROR } storage_state_t;
static struct {
    bool initialized;
    bool connected;
    bool candidate;
    bool usb_installed;
    bool msc_installed;
    bool disconnecting;
    sdmmc_card_t *local_card;
    sdmmc_card_t *usb_card;
    tinyusb_msc_storage_handle_t storage;
    storage_state_t state;
    esp_err_t error;
    int64_t changed_us;
    int64_t transition_us;
    void (*prepare)(void);
    void (*resume)(void);
} s_ctx;

esp_err_t usb_storage_init(void)
{
    esp_err_t err = sd_access_guard_init();
    if (err != ESP_OK) return err;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << NOMADCAST_PIN_USB_VBUS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    s_ctx.initialized = true;
    s_ctx.candidate = gpio_get_level(NOMADCAST_PIN_USB_VBUS) == NOMADCAST_USB_DETECT_ACTIVE_LEVEL;
    s_ctx.changed_us = esp_timer_get_time();
    return ESP_OK;
}

bool usb_storage_connected(void)
{
    if (!s_ctx.initialized) return false;
    bool value = gpio_get_level(NOMADCAST_PIN_USB_VBUS) == NOMADCAST_USB_DETECT_ACTIVE_LEVEL;
    int64_t now = esp_timer_get_time();
    if (value != s_ctx.candidate) {
        s_ctx.candidate = value;
        s_ctx.changed_us = now;
    }
    if (now - s_ctx.changed_us >= 150000) s_ctx.connected = value;
    return s_ctx.connected;
}

void usb_storage_set_callbacks(void (*prepare)(void), void (*resume)(void))
{
    s_ctx.prepare = prepare;
    s_ctx.resume = resume;
}

esp_err_t usb_storage_mount(void)
{
    if (s_ctx.local_card) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.width = 1;
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    return esp_vfs_fat_sdmmc_mount(SD_PATH, &host, &slot, &config, &s_ctx.local_card);
}

bool usb_storage_local_available(void) { return s_ctx.local_card && s_ctx.state == STORAGE_LOCAL; }
bool usb_storage_has_card(void) { return s_ctx.local_card || s_ctx.usb_card; }
bool usb_storage_enabled(void) { return s_ctx.state == STORAGE_USB || s_ctx.state == STORAGE_ENTERING; }
bool usb_storage_busy(void) { return s_ctx.state == STORAGE_ENTERING || s_ctx.state == STORAGE_LEAVING; }
esp_err_t usb_storage_last_error(void) { return s_ctx.error; }

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
        sdmmc_host_deinit();
        free(card);
        return ret;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC card init failed: %s (no card? check pins/power)", esp_err_to_name(ret));
        sdmmc_host_deinit();
        free(card);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    *out_card = card;
    return ESP_OK;
}


static esp_err_t start_usb(void)
{
    esp_err_t err = sdmmc_init(&s_ctx.usb_card);
    if (err != ESP_OK) return err;
    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = { .base_path = NULL, .config.max_files = 5, .format_flags = 0 },
    };
    storage_cfg.medium.card = s_ctx.usb_card;
    tinyusb_msc_driver_config_t msc_cfg = {0};
    msc_cfg.user_flags.auto_mount_off = 1;
    err = tinyusb_msc_install_driver(&msc_cfg);
    if (err != ESP_OK) goto fail;
    s_ctx.msc_installed = true;
    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_ctx.storage);
    if (err != ESP_OK) goto fail;
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &descriptor_config;
    cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    cfg.descriptor.string = string_desc_arr;
    cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) goto fail;
    s_ctx.usb_installed = true;
    s_ctx.disconnecting = false;
    return ESP_OK;
fail:
    if (s_ctx.storage) { tinyusb_msc_delete_storage(s_ctx.storage); s_ctx.storage = NULL; }
    if (s_ctx.msc_installed) { tinyusb_msc_uninstall_driver(); s_ctx.msc_installed = false; }
    sdmmc_host_deinit();
    free(s_ctx.usb_card);
    s_ctx.usb_card = NULL;
    return err;
}

static esp_err_t stop_usb(void)
{
    esp_err_t err;
    if (s_ctx.usb_installed && !s_ctx.disconnecting) {
        tud_disconnect();
        s_ctx.disconnecting = true;
    }
    if (s_ctx.storage) {
        /* The USB task stays alive until its deferred SD writes finish. */
        err = tinyusb_msc_delete_storage(s_ctx.storage);
        if (err != ESP_OK) return err;
        s_ctx.storage = NULL;
    }
    if (s_ctx.usb_installed) {
        err = tinyusb_driver_uninstall();
        if (err != ESP_OK) return err;
        s_ctx.usb_installed = false;
    }
    if (s_ctx.msc_installed) {
        err = tinyusb_msc_uninstall_driver();
        if (err != ESP_OK) return err;
        s_ctx.msc_installed = false;
    }
    if (s_ctx.usb_card) {
        sdmmc_host_deinit();
        free(s_ctx.usb_card);
        s_ctx.usb_card = NULL;
    }
    return ESP_OK;
}

static void restore_local(void)
{
    esp_err_t err = usb_storage_mount();
    if (err != ESP_OK) {
        s_ctx.error = err;
        s_ctx.state = STORAGE_ERROR;
        return; /* Keep all app SD access blocked until remount succeeds. */
    }
    s_ctx.state = STORAGE_LOCAL;
    sd_access_guard_block(false);
    if (s_ctx.resume) s_ctx.resume();
}

esp_err_t usb_storage_set_enabled(bool enabled)
{
    if (usb_storage_busy()) return ESP_ERR_INVALID_STATE;
    s_ctx.error = ESP_OK;
    if (enabled) {
        if (s_ctx.state == STORAGE_USB) return ESP_OK;
        if (!usb_storage_connected() || !usb_storage_local_available()) return ESP_ERR_INVALID_STATE;
        s_ctx.state = STORAGE_ENTERING;
        s_ctx.transition_us = esp_timer_get_time();
        if (s_ctx.prepare) s_ctx.prepare();
        sd_access_guard_block(true);
    } else if (s_ctx.state != STORAGE_LOCAL) {
        s_ctx.state = STORAGE_LEAVING;
    }
    return ESP_OK;
}

void usb_storage_process(void)
{
    bool connected = usb_storage_connected();
    if (s_ctx.state == STORAGE_USB && !connected) s_ctx.state = STORAGE_LEAVING;
    if (s_ctx.state == STORAGE_ENTERING) {
        if (!connected || esp_timer_get_time() - s_ctx.transition_us > 5000000) {
            s_ctx.error = connected ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
            restore_local();
            return;
        }
        if (!sd_access_guard_idle()) return;
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_PATH, s_ctx.local_card);
        /* IDF consumes the mount context/card even if VFS unregister fails. */
        s_ctx.local_card = NULL;
        if (err == ESP_OK) err = start_usb();
        if (err != ESP_OK) {
            s_ctx.error = err;
            restore_local();
        } else {
            s_ctx.state = STORAGE_USB;
        }
    } else if (s_ctx.state == STORAGE_LEAVING) {
        esp_err_t err = stop_usb();
        if (err != ESP_OK) {
            if (err != ESP_ERR_INVALID_STATE) s_ctx.error = err;
            return; /* A queued write is still draining; retry next tick. */
        }
        s_ctx.error = ESP_OK;
        restore_local();
    }
}
