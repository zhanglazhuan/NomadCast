#include "nomadcast.h"
/* PCB V1.2 key/display test. Pin definitions: nomadcast.h.
 * USB_DETE is active LOW; it is separate from the USB wake input.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "lvgl.h"

static const char *TAG = "t_key";

/* ========================================================================
 * Pin Definitions
 * ======================================================================== */

#define BTN_VOL_DOWN NOMADCAST_PIN_KEY_VOL_DOWN
#define BTN_VOL_UP NOMADCAST_PIN_KEY_VOL_UP
#define BTN_STOP NOMADCAST_PIN_KEY_STOP
#define BTN_ONOFF NOMADCAST_PIN_KEY_POWER
#define PIN_HP_DET NOMADCAST_PIN_HP_DETECT
#define PIN_USB_VBUS NOMADCAST_PIN_USB_VBUS

#define PIN_EN_POWER NOMADCAST_PIN_AP_POWER
#define PIN_LCD_POWER NOMADCAST_PIN_LCD_POWER
#define PIN_BACKLIGHT NOMADCAST_PIN_LCD_BL
#define PIN_LCD_RST NOMADCAST_PIN_LCD_RST
#define PIN_TP_INT NOMADCAST_PIN_TP_INT

#define PIN_SPI_SCK NOMADCAST_PIN_SPI_SCK
#define PIN_SPI_MOSI NOMADCAST_PIN_SPI_MOSI
#define PIN_SPI_MISO NOMADCAST_PIN_SPI_MISO
#define PIN_LCD_CS NOMADCAST_PIN_LCD_CS
#define PIN_LCD_DC NOMADCAST_PIN_LCD_DC

#define LCD_W           240
#define LCD_H           320
#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (20 * 1000 * 1000)

/* ========================================================================
 * Screen Event Queue — 事件进队列，在 LVGL 线程统一刷新（LVGL 非线程安全）。
 * 任意任务都能调用 ui_post_*()，不关心 LVGL 上下文。
 * ======================================================================== */

typedef enum { UI_MSG_LOG = 0, UI_MSG_USB_STATE } ui_msg_type_t;

typedef struct {
    ui_msg_type_t type;
    int           level;          /* UI_MSG_USB_STATE: 1=已插入 */
    char          text[28];       /* UI_MSG_LOG: 一行事件 */
} ui_msg_t;

static QueueHandle_t s_ui_queue = NULL;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void ui_post(ui_msg_type_t type, int level, const char *text)
{
    if (!s_ui_queue) return;
    ui_msg_t m = { .type = type, .level = level };
    if (text) {
        snprintf(m.text, sizeof(m.text), "%s", text);   /* 自动截断，避免 strncpy 截断警告 */
    }
    xQueueSend(s_ui_queue, &m, 0);   /* 队列满则丢弃 — 测试工具不必较真 */
}

static void ui_post_log(const char *fmt, ...)
{
    char line[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ui_post(UI_MSG_LOG, 0, line);
}

static void ui_post_usb(int level)
{
    ui_post(UI_MSG_USB_STATE, level, NULL);
}

/* ========================================================================
 * Button Handling
 * ======================================================================== */

#define DEBOUNCE_MS     50

static QueueHandle_t gpio_evt_queue = NULL;
static int s_last_hp_level = -1;   /* 上一次稳定的耳机电平，-1=未初始化 */

/* 防抖读取：要求电平稳定 DEBOUNCE_MS 才返回，过滤机械触点抖动 */
static int debounced_level(gpio_num_t pin)
{
    int level = gpio_get_level(pin);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        int l = gpio_get_level(pin);
        if (l == level) {
            return level;   /* 稳定 DEBOUNCE_MS，返回 */
        }
        level = l;           /* 还在抖，重新计时 */
    }
}

static const char *btn_name(uint32_t gpio_num)
{
    switch (gpio_num) {
        case BTN_VOL_DOWN: return "Vol-";
        case BTN_VOL_UP:   return "Vol+";
        case BTN_STOP:     return "Stop";
        case BTN_ONOFF:    return "OnOff";
        default:           return "Unknown";
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void btn_task(void *arg)
{
    (void)arg;
    uint32_t gpio_num;

    /* 上电等待耳机检测电路稳定（~700ms 启动抖动），以稳定电平为基线，
     * 并丢弃启动期间积压的抖动事件 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_last_hp_level = debounced_level(PIN_HP_DET);
    while (xQueueReceive(gpio_evt_queue, &gpio_num, 0) == pdTRUE) { /* discard */ }
    ESP_LOGI(TAG, "HP baseline: %s", s_last_hp_level ? "INSERTED" : "REMOVED");
    ui_post_log("t=%lus HP %s", (unsigned long)uptime_s(),
                s_last_hp_level ? "insert" : "remove");

    while (1) {
        if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY)) {
            /* 耳机插入检测：双边沿 + 防抖 + 只在电平变化时打印 */
            if (gpio_num == PIN_HP_DET) {
                int level = debounced_level(PIN_HP_DET);
                if (level != s_last_hp_level) {
                    s_last_hp_level = level;
                    ESP_LOGI(TAG, "[HP] headphone %s (GPIO14=%d)",
                             level ? "INSERTED" : "REMOVED", level);
                    ui_post_log("t=%lus HP %s", (unsigned long)uptime_s(),
                                level ? "insert" : "remove");
                }
                continue;
            }

            /* 按键：防抖 + 只认高电平（按下） */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(gpio_num) != 1) continue;

            ESP_LOGI(TAG, "[BTN] %s pressed (GPIO%d)", btn_name(gpio_num), (int)gpio_num);
            ui_post_log("t=%lus %s", (unsigned long)uptime_s(), btn_name(gpio_num));
        }
    }
}

/* ========================================================================
 * USB VBUS (GPIO18) Monitor — 轮询 + 防抖，只记录电平变化
 * ======================================================================== */

static int s_last_vbus = -1;

static void usb_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        int connected = debounced_level(PIN_USB_VBUS) == NOMADCAST_USB_DETECT_ACTIVE_LEVEL;
        if (connected != s_last_vbus) {
            s_last_vbus = connected;
            ui_post_usb(connected);
            ui_post_log("t=%lus USB: %s", (unsigned long)uptime_s(), connected ? "connected" : "disconnected");
            ESP_LOGI(TAG, "USB_DETE GPIO%d=%d: %s", PIN_USB_VBUS,
                     gpio_get_level(PIN_USB_VBUS), connected ? "INSERTED" : "REMOVED");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void usb_vbus_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = BIT64(PIN_USB_VBUS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

/* ========================================================================
 * Screen UI — 事件进队列，在 LVGL 线程统一刷新（LVGL 非线程安全）
 * ======================================================================== */

#define LOG_MAX_LINES  12
static char s_log_ring[LOG_MAX_LINES][28];
static int  s_log_count = 0;

static lv_obj_t *s_usb_state_label;   /* font 20 — 大字号 USB 状态 */
static lv_obj_t *s_usb_raw_label;     /* font 14 — GPIO18 原始电平 */
static lv_obj_t *s_log_label;         /* font 14 — 事件滚动日志 */

static void ui_apply_events(void)
{
    ui_msg_t m;
    while (xQueueReceive(s_ui_queue, &m, 0) == pdTRUE) {
        if (m.type == UI_MSG_USB_STATE) {
            bool on = (m.level == 1);
            lv_label_set_text(s_usb_state_label,
                              on ? "USB: CONNECTED" : "USB: DISCONNECTED");
            lv_obj_set_style_text_color(s_usb_state_label,
                                        on ? lv_color_hex(0x2ECC40) : lv_color_hex(0xFF4136), 0);
            lv_label_set_text_fmt(s_usb_raw_label, "USB=%d  t=%lus",
                                  m.level, (unsigned long)uptime_s());
        } else {
            if (s_log_count >= LOG_MAX_LINES) {
                memmove(s_log_ring, s_log_ring + 1,
                        sizeof(s_log_ring) - sizeof(s_log_ring[0]));
                s_log_count = LOG_MAX_LINES - 1;
            }
            strncpy(s_log_ring[s_log_count], m.text, sizeof(s_log_ring[0]) - 1);
            s_log_ring[s_log_count][sizeof(s_log_ring[0]) - 1] = '\0';
            s_log_count++;

            char buf[LOG_MAX_LINES * 30];
            buf[0] = '\0';
            for (int i = 0; i < s_log_count; i++) {
                strcat(buf, s_log_ring[i]);
                if (i < s_log_count - 1) strcat(buf, "\n");
            }
            lv_label_set_text(s_log_label, buf);
        }
    }

    lv_label_set_text_fmt(s_usb_raw_label, "USB_DETE GPIO%d=%d (LOW=USB)",
                          PIN_USB_VBUS, gpio_get_level(PIN_USB_VBUS));
}

static void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_usb_state_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_usb_state_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_usb_state_label, lv_color_white(), 0);
    lv_obj_set_pos(s_usb_state_label, 12, 6);
    lv_label_set_text(s_usb_state_label, "USB: --");

    s_usb_raw_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_usb_raw_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_usb_raw_label, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(s_usb_raw_label, 12, 38);
    lv_label_set_text(s_usb_raw_label, "USB_DETE=?");

    s_log_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_log_label, lv_color_white(), 0);
    lv_obj_set_pos(s_log_label, 12, 64);
    lv_obj_set_width(s_log_label, 216);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_log_label, "");
}

/* ========================================================================
 * Display Init — ST7789 via SPI2 (esp_lcd)，沿用 main/NomadCast.cpp 已验证配置
 * ======================================================================== */

static esp_lcd_panel_handle_t s_panel;

static void display_init(void)
{
    spi_bus_config_t spi_cfg = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 20 * 2,   /* 匹配 flush 缓冲高度 */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = SPI_FREQ_HZ, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                             &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

/* HW 复位 (GPIO40 与 GT911 共用) */
static void hw_reset(void)
{
    gpio_config_t cfg = { .pin_bit_mask = BIT64(PIN_LCD_RST) | BIT64(PIN_TP_INT),
                          .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(PIN_TP_INT, 0); gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_INT, 1);
    gpio_config_t in_cfg = { .pin_bit_mask = BIT64(PIN_TP_INT), .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* ---- LVGL ---- */

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;

    /* RGB565 byte swap — LVGL big-endian → ST7789 SPI little-endian */
    lv_draw_sw_rgb565_swap(px, w * h);

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x1 + w, area->y1 + h, px);
    lv_display_flush_ready(disp);
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(1); }

static void lvgl_init(void)
{
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_user_data(disp, s_panel);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    size_t buf_sz = LCD_W * 20 * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                          lv_palette_main(LV_PALETTE_RED), LV_THEME_DEFAULT_DARK,
                          &lv_font_montserrat_14);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb, .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);   /* 1000 us = 1 ms */
}

/* ========================================================================
 * Button GPIO Init
 * ======================================================================== */

static void btn_init(void)
{
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    ESP_ERROR_CHECK(gpio_evt_queue == NULL ? ESP_FAIL : ESP_OK);

    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_POSEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_VOL_DOWN) |
                        (1ULL << BTN_VOL_UP)   |
                        (1ULL << BTN_STOP)     |
                        (1ULL << BTN_ONOFF),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* 耳机插入检测 (AMP_EN=GPIO14)：双边沿 + 悬空（板子自带上下拉，ESP32 别抢） */
    gpio_config_t hp_conf = {
        .intr_type    = GPIO_INTR_ANYEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_HP_DET),
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&hp_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_VOL_DOWN, gpio_isr_handler, (void *)BTN_VOL_DOWN));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_VOL_UP,   gpio_isr_handler, (void *)BTN_VOL_UP));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_STOP,     gpio_isr_handler, (void *)BTN_STOP));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_ONOFF,    gpio_isr_handler, (void *)BTN_ONOFF));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_HP_DET,   gpio_isr_handler, (void *)PIN_HP_DET));

    xTaskCreate(btn_task, "btn_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Buttons ready — Vol-:IO6, Vol+:IO4, Stop:IO7, OnOff:IO5, HP:IO14");
}

/* ========================================================================
 * Main
 * ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== t_key: Button + USB VBUS Event Test ===");

    /* 屏幕事件队列 — 先建，任意任务都可 ui_post() */
    s_ui_queue = xQueueCreate(16, sizeof(ui_msg_t));

    /* 电源: 全板 + 屏幕 + 背光 */
    gpio_config_t pwr = {
        .pin_bit_mask = BIT64(PIN_EN_POWER) | BIT64(PIN_LCD_POWER) |
                        BIT64(PIN_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(PIN_EN_POWER, 1);
    gpio_set_level(PIN_LCD_POWER, 1);
    gpio_set_level(PIN_BACKLIGHT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "HW: EN_POWER(46)=H LCD_POWER(21)=H BL(39)=H");

    hw_reset();
    display_init();
    lvgl_init();
    ui_init();
    usb_vbus_init();

    btn_init();
    xTaskCreate(usb_monitor_task, "usb_monitor", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Press keys / plug USB — events shown on screen + serial");

    while (1) {
        ui_apply_events();      /* 在 LVGL 线程处理所有屏幕事件 */
        uint32_t delay = lv_timer_handler();
        if (delay < 5) delay = 5;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
