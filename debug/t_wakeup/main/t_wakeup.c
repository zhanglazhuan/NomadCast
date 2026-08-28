/*
 * NomadCast — USB wakeup diagnostic (Leisound/NomadCast V1.1)
 *
 * Signal under test:
 *   USB +5V -> R11/R14 divider -> D7 -> WAKEUP_MCU -> GPIO3/ADC1_CH2
 *
 * Test modes selected at boot:
 *   Power key released: reproduce production firmware (GPIO3 pull-down ON)
 *   Power key held:     comparison test (GPIO3 pull-down OFF)
 *
 * Screen colors:
 *   BLUE    normal boot, production pull-down mode
 *   YELLOW  normal boot, no-pull comparison mode
 *   GREEN   woke from GPIO3 after USB insertion
 *   MAGENTA woke manually from the power key (GPIO5)
 *   RED     invalid/false wake or wake configuration error
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "t_wakeup";

/* Wake inputs. */
#define PIN_USB_WAKE       GPIO_NUM_3
#define PIN_POWER_KEY      GPIO_NUM_5
#define USB_ADC_UNIT       ADC_UNIT_1
#define USB_ADC_CHANNEL    ADC_CHANNEL_2

/* Display and board power pins from NomadCast V1.1. */
#define PIN_AP_POWER       GPIO_NUM_46
#define PIN_LCD_POWER      GPIO_NUM_43
#define PIN_LCD_BACKLIGHT  GPIO_NUM_12
#define PIN_LCD_CS         GPIO_NUM_48
#define PIN_LCD_DC         GPIO_NUM_47
#define PIN_LCD_RST        GPIO_NUM_40
#define PIN_SPI_SCK        GPIO_NUM_21
#define PIN_SPI_MOSI       GPIO_NUM_14
#define PIN_SPI_MISO       GPIO_NUM_13

#define LCD_HOST           SPI2_HOST
#define LCD_WIDTH          240
#define LCD_HEIGHT         320
#define LCD_STRIP_HEIGHT   20
#define LCD_SPI_HZ         (20 * 1000 * 1000)

#define ADC_SAMPLE_COUNT          32
#define USB_PRESENT_MIN_MV       500
#define USB_UNPLUGGED_MAX_MV     300
#define MIN_OBSERVATION_SAMPLES    5
#define UNPLUG_STABLE_SAMPLES      3

typedef enum {
    WAKE_MODE_PULLDOWN,
    WAKE_MODE_NO_PULL,
} wake_mode_t;

#define RTC_TEST_STATE_MAGIC 0x57414b45U /* "WAKE" */

RTC_DATA_ATTR static uint32_t s_rtc_test_state_magic;
RTC_DATA_ATTR static wake_mode_t s_rtc_sleep_mode;

typedef struct {
    int raw;
    int millivolts;
    int digital_level;
    int valid_samples;
    bool valid;
    bool calibrated;
} usb_sample_t;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_lcd_strip;

static const char *wake_mode_name(wake_mode_t mode)
{
    return mode == WAKE_MODE_PULLDOWN ? "PULLDOWN" : "NO_PULL";
}

static uint16_t rgb565_wire(uint8_t red, uint8_t green, uint8_t blue)
{
    uint16_t color = (uint16_t)(((red & 0xf8U) << 8) |
                                ((green & 0xfcU) << 3) |
                                (blue >> 3));
    return (uint16_t)((color << 8) | (color >> 8));
}

static void display_power_configure(void)
{
    gpio_config_t power_cfg = {
        .pin_bit_mask = (1ULL << PIN_AP_POWER) |
                        (1ULL << PIN_LCD_POWER) |
                        (1ULL << PIN_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&power_cfg));

    gpio_set_level(PIN_LCD_BACKLIGHT, 0);
    gpio_set_level(PIN_AP_POWER, 1);
    gpio_set_level(PIN_LCD_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
}

static void display_init(void)
{
    display_power_configure();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_STRIP_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_lcd_strip = heap_caps_malloc(LCD_WIDTH * LCD_STRIP_HEIGHT * sizeof(uint16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(s_lcd_strip ? ESP_OK : ESP_ERR_NO_MEM);
}

static void display_fill(uint8_t red, uint8_t green, uint8_t blue)
{
    uint16_t color = rgb565_wire(red, green, blue);
    for (int i = 0; i < LCD_WIDTH * LCD_STRIP_HEIGHT; ++i) {
        s_lcd_strip[i] = color;
    }

    for (int y = 0; y < LCD_HEIGHT; y += LCD_STRIP_HEIGHT) {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            s_panel, 0, y, LCD_WIDTH, y + LCD_STRIP_HEIGHT, s_lcd_strip));
    }
    gpio_set_level(PIN_LCD_BACKLIGHT, 1);
}

static void display_off(void)
{
    gpio_set_level(PIN_LCD_BACKLIGHT, 0);
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    gpio_set_level(PIN_LCD_POWER, 0);
}

static void usb_gpio_configure(bool pull_down)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_USB_WAKE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void usb_adc_pad_configure(bool pull_down)
{
    /* gpio_config() selects the digital pad and disconnects the ADC path.
     * Restore analog mode before every ADC batch, then configure the RTC-pad
     * pull resistor without switching the pin back to digital mode. */
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, USB_ADC_CHANNEL, &channel_cfg));
    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(PIN_USB_WAKE));
    if (pull_down) {
        ESP_ERROR_CHECK(rtc_gpio_pulldown_en(PIN_USB_WAKE));
    } else {
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(PIN_USB_WAKE));
    }
}

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = USB_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    usb_adc_pad_configure(false);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = USB_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali);
    s_adc_calibrated = (err == ESP_OK);
    if (!s_adc_calibrated) {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s); millivolts are approximate",
                 esp_err_to_name(err));
    }
}

static void adc_deinit(void)
{
    if (s_adc_calibrated) {
        adc_cali_delete_scheme_curve_fitting(s_adc_cali);
        s_adc_cali = NULL;
        s_adc_calibrated = false;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}

static usb_sample_t usb_sample(bool pull_down)
{
    usb_sample_t sample = {
        .raw = 0,
        .millivolts = 0,
        .digital_level = 0,
        .valid_samples = 0,
        .valid = false,
        .calibrated = s_adc_calibrated,
    };

    usb_adc_pad_configure(pull_down);
    vTaskDelay(pdMS_TO_TICKS(30));

    int64_t raw_sum = 0;
    int valid_samples = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, USB_ADC_CHANNEL, &raw) == ESP_OK) {
            raw_sum += raw;
            ++valid_samples;
        }
    }
    if (valid_samples > 0) {
        sample.valid_samples = valid_samples;
        sample.raw = (int)(raw_sum / valid_samples);
        if (s_adc_calibrated) {
            if (adc_cali_raw_to_voltage(s_adc_cali, sample.raw,
                                        &sample.millivolts) != ESP_OK) {
                sample.millivolts = 0;
                sample.calibrated = false;
            }
        }
        if (!sample.calibrated) {
            /* Approximate 12 dB full scale, used only if eFuse calibration fails. */
            sample.millivolts = sample.raw * 3100 / 4095;
        }
        sample.valid = valid_samples >= (ADC_SAMPLE_COUNT / 2);
    }

    /* ADC sampling selects the analog path. Re-select the digital input before
     * reading the logic level under the same pull configuration. */
    usb_gpio_configure(pull_down);
    vTaskDelay(pdMS_TO_TICKS(10));
    sample.digital_level = gpio_get_level(PIN_USB_WAKE);
    return sample;
}

static void log_sample_pair(const usb_sample_t *no_pull,
                            const usb_sample_t *pull_down)
{
    ESP_LOGI(TAG,
             "GPIO3  NO_PULL: %s n=%2d raw=%4d %4dmV D=%d | "
             "PULLDOWN: %s n=%2d raw=%4d %4dmV D=%d%s",
             no_pull->valid ? "OK " : "BAD", no_pull->valid_samples,
             no_pull->raw, no_pull->millivolts, no_pull->digital_level,
             pull_down->valid ? "OK " : "BAD", pull_down->valid_samples,
             pull_down->raw, pull_down->millivolts, pull_down->digital_level,
             no_pull->calibrated ? "" : " (mV approximate)");
}

static wake_mode_t select_wake_mode(void)
{
    rtc_gpio_deinit(PIN_POWER_KEY);
    gpio_config_t key_cfg = {
        .pin_bit_mask = 1ULL << PIN_POWER_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key_cfg));
    vTaskDelay(pdMS_TO_TICKS(80));
    return gpio_get_level(PIN_POWER_KEY) ? WAKE_MODE_NO_PULL : WAKE_MODE_PULLDOWN;
}

static void monitor_forever(void)
{
    ESP_LOGW(TAG, "Monitor-only mode; reset the board to restart the sleep test");
    while (true) {
        usb_sample_t no_pull = usb_sample(false);
        usb_sample_t pull_down = usb_sample(true);
        log_sample_pair(&no_pull, &pull_down);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wait_until_usb_is_unplugged(void)
{
    int sample_count = 0;
    int low_streak = 0;

    ESP_LOGI(TAG, "Observe both GPIO3 readings, then UNPLUG the USB cable");
    ESP_LOGI(TAG, "The serial monitor will disconnect; battery must remain connected");

    while (true) {
        usb_sample_t no_pull = usb_sample(false);
        usb_sample_t pull_down = usb_sample(true);
        log_sample_pair(&no_pull, &pull_down);

        ++sample_count;
        /* Physical presence must not be inferred only from the loaded reading:
         * that would turn the suspected pull-down voltage collapse into a false
         * "USB unplugged" result. Both measurements must be valid and low. */
        bool unplugged = no_pull.valid && pull_down.valid &&
                         no_pull.millivolts <= USB_UNPLUGGED_MAX_MV &&
                         pull_down.millivolts <= USB_UNPLUGGED_MAX_MV &&
                         pull_down.digital_level == 0;
        if (sample_count >= MIN_OBSERVATION_SAMPLES && unplugged) {
            ++low_streak;
        } else {
            low_streak = 0;
        }

        if (low_streak >= UNPLUG_STABLE_SAMPLES) {
            ESP_LOGI(TAG, "USB absent for %d consecutive samples", low_streak);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static bool usb_signal_is_present(const usb_sample_t *no_pull)
{
    return no_pull->valid && no_pull->millivolts >= USB_PRESENT_MIN_MV;
}

static esp_err_t enter_deep_sleep(wake_mode_t mode)
{
    bool use_pull_down = (mode == WAKE_MODE_PULLDOWN);
    usb_gpio_configure(use_pull_down);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (gpio_get_level(PIN_USB_WAKE) != 0) {
        ESP_LOGE(TAG, "GPIO3 is already HIGH with USB absent (%s mode); refusing sleep",
                 use_pull_down ? "PULLDOWN" : "NO_PULL");
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t wake_mask = (1ULL << PIN_USB_WAKE) | (1ULL << PIN_POWER_KEY);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(
        wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_ext1_wakeup failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = rtc_gpio_pullup_dis(PIN_USB_WAKE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot disable GPIO3 RTC pull-up: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (use_pull_down) {
        err = rtc_gpio_pulldown_en(PIN_USB_WAKE);
    } else {
        err = rtc_gpio_pulldown_dis(PIN_USB_WAKE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot configure GPIO3 RTC pull-down: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = rtc_gpio_pullup_dis(PIN_POWER_KEY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot disable GPIO5 RTC pull-up: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = rtc_gpio_pulldown_en(PIN_POWER_KEY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot enable GPIO5 RTC pull-down: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Deep sleep restarts the application. Preserve the mode that armed this
     * particular test so the wake report cannot mislabel a NO_PULL result as
     * the default PULLDOWN mode. */
    s_rtc_test_state_magic = RTC_TEST_STATE_MAGIC;
    s_rtc_sleep_mode = mode;

    adc_deinit();

    ESP_LOGI(TAG, "Entering deep sleep: GPIO3 USB wake, GPIO5 rescue, mode=%s",
             wake_mode_name(mode));
    ESP_LOGI(TAG, "Now plug the USB cable in; GREEN screen means GPIO3 woke the MCU");
    vTaskDelay(pdMS_TO_TICKS(250));
    display_off();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
    return ESP_FAIL; /* Not reached. */
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    uint64_t ext1_status = wake_cause == ESP_SLEEP_WAKEUP_EXT1
                         ? esp_sleep_get_ext1_wakeup_status()
                         : 0;

    bool stored_mode_valid = wake_cause == ESP_SLEEP_WAKEUP_EXT1 &&
                             s_rtc_test_state_magic == RTC_TEST_STATE_MAGIC &&
                             (s_rtc_sleep_mode == WAKE_MODE_PULLDOWN ||
                              s_rtc_sleep_mode == WAKE_MODE_NO_PULL);

    rtc_gpio_deinit(PIN_USB_WAKE);
    rtc_gpio_deinit(PIN_POWER_KEY);
    /* Select a mode only for a new test. EXT1 wake is a restart, so reuse the
     * RTC-retained mode that actually armed the just-completed trial. */
    wake_mode_t mode = stored_mode_valid ? s_rtc_sleep_mode
                                         : select_wake_mode();

    display_init();
    adc_init();

    bool woke_by_usb = (ext1_status & (1ULL << PIN_USB_WAKE)) != 0;
    bool woke_by_key = (ext1_status & (1ULL << PIN_POWER_KEY)) != 0;

    if (woke_by_usb) {
        display_fill(0, 255, 0);       /* GREEN */
    } else if (woke_by_key) {
        display_fill(255, 0, 255);     /* MAGENTA */
    } else if (mode == WAKE_MODE_NO_PULL) {
        display_fill(255, 180, 0);     /* YELLOW */
    } else {
        display_fill(0, 80, 255);      /* BLUE */
    }

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "NomadCast GPIO3 USB wakeup diagnostic");
    ESP_LOGI(TAG, "wake_cause=%d ext1_status=0x%llx mode=%s",
             (int)wake_cause, (unsigned long long)ext1_status,
             wake_mode_name(mode));
    ESP_LOGI(TAG, "Hold GPIO5 during boot to select NO_PULL comparison mode");
    ESP_LOGI(TAG, "====================================================");

    /* Give USB Serial/JTAG time to enumerate again after a successful wake. */
    if (woke_by_usb) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        usb_sample_t no_pull = usb_sample(false);
        usb_sample_t pull_down = usb_sample(true);
        log_sample_pair(&no_pull, &pull_down);

        if (!no_pull.valid) {
            ESP_LOGE(TAG, "Cannot validate USB wake: NO_PULL ADC batch is invalid");
            display_fill(255, 0, 0);   /* RED */
            monitor_forever();
        }
        if (!usb_signal_is_present(&no_pull)) {
            ESP_LOGE(TAG, "GPIO3 caused EXT1 wake, but USB voltage is absent: false/floating wake");
            display_fill(255, 0, 0);   /* RED */
            monitor_forever();
        }
        if (mode == WAKE_MODE_NO_PULL) {
            /* With no pull resistor, this same pin cannot distinguish a real
             * USB drive level from a floating/noisy level. The user's cable
             * insertion timing is the independent evidence for this trial. */
            ESP_LOGW(TAG,
                     "RESULT: GPIO3 caused EXT1 and is HIGH in NO_PULL mode; "
                     "confirm it followed the USB insertion (floating cannot be excluded)");
        } else {
            ESP_LOGI(TAG,
                     "PASS: GPIO3 caused EXT1 and wake voltage is present; tested mode=%s",
                     wake_mode_name(mode));
        }
    } else if (woke_by_key) {
        ESP_LOGW(TAG, "Manual rescue wake from GPIO5");
    }

    wait_until_usb_is_unplugged();

    /* Re-apply the selected sleep mode and reject an already-HIGH/floating
     * input before arming EXT1. */
    esp_err_t err = enter_deep_sleep(mode);
    ESP_LOGE(TAG, "Cannot start wake test: %s", esp_err_to_name(err));
    display_fill(255, 0, 0);           /* RED */
    monitor_forever();
}
