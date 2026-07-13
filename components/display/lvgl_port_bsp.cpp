/*
 * LVGL v9 Port Layer for ST7789V (240x320, 4-Wire SPI)
 *
 * Adapted from reference lvgl_port_bsp (which targeted SH8601 QSPI AMOLED).
 * Retains the same architecture: dual DMA buffers, flush semaphore,
 * LVGL mutex, tick timer, and LVGL task loop.
 *
 * Display: ST7789V via SPI2 (ESP-IDF esp_lcd_panel_st7789 driver)
 * Touch:   GT911 via I2C0 (touch_bsp component)
 * Board:   Leisound V1
 */

#include <stdio.h>
#include "lvgl_port_bsp.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "board/leisound_v1.h"
#include "i2c_bsp.h"
#include "touch_bsp.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define LCD_HOST             LEISOUND_LCD_HOST
#define LCD_H_RES            LEISOUND_LCD_H_RES
#define LCD_V_RES            LEISOUND_LCD_V_RES
#define LCD_BIT_PER_PIXEL    LEISOUND_LCD_BIT_PER_PIXEL
#define LVGL_BUF_HEIGHT      LEISOUND_LVGL_BUF_HEIGHT
#define LVGL_TICK_PERIOD_MS  LEISOUND_LVGL_TICK_PERIOD_MS
#define LVGL_TASK_MAX_DELAY  LEISOUND_LVGL_TASK_MAX_DELAY
#define LVGL_TASK_MIN_DELAY  LEISOUND_LVGL_TASK_MIN_DELAY
#define LVGL_TASK_STACK_SIZE LEISOUND_LVGL_TASK_STACK
#define LVGL_TASK_PRIORITY   LEISOUND_LVGL_TASK_PRIORITY

#define LCD_CS_PIN           LEISOUND_PIN_LCD_CS
#define LCD_DC_PIN           LEISOUND_PIN_LCD_DC
#define LCD_RST_PIN          LEISOUND_PIN_LCD_RST
#define SPI_SCK_PIN          LEISOUND_PIN_SPI_SCK
#define SPI_MOSI_PIN         LEISOUND_PIN_SPI_MOSI
#define SPI_MISO_PIN         LEISOUND_PIN_SPI_MISO

#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (LCD_H_RES * LVGL_BUF_HEIGHT * BYTES_PER_PIXEL)

/* ========================================================================
 * Static State
 * ======================================================================== */

static esp_lcd_panel_handle_t    panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle    = NULL;
static SemaphoreHandle_t         lvgl_mux     = NULL;
static SemaphoreHandle_t         flush_done_semaphore = NULL;

/* Global I2C bus and touch panel instances (created in main, used by LVGL port) */
static I2cMasterBus  *g_i2c_bus   = NULL;
static LcdTouchPanel *g_touch_pnl = NULL;

/* ========================================================================
 * External accessor — main sets these before calling Lvgl_PortInit()
 * ======================================================================== */

void lvgl_port_set_i2c_bus(I2cMasterBus *bus)   { g_i2c_bus = bus; }
void lvgl_port_set_touch_panel(LcdTouchPanel *tp) { g_touch_pnl = tp; }

/* ========================================================================
 * LCD Panel Initialization (ST7789V via 4-Wire SPI)
 * ======================================================================== */

static bool Lcd_OnColorTransDone(esp_lcd_panel_io_handle_t panel_io,
                                  esp_lcd_panel_io_event_data_t *edata,
                                  void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_semaphore, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}

static void Lvgl_LcdPanelInit(int lcd_host)
{
    /* ---- SPI bus ---- */
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num      = SPI_SCK_PIN;
    buscfg.mosi_io_num      = SPI_MOSI_PIN;
    buscfg.miso_io_num      = SPI_MISO_PIN;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8 + 16;
    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t) lcd_host, &buscfg, SPI_DMA_CH_AUTO));

    /* ---- Panel IO (4-wire SPI, 8-bit commands/params) ---- */
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num                   = LCD_CS_PIN;
    io_config.dc_gpio_num                   = LCD_DC_PIN;
    io_config.spi_mode                      = 0;
    io_config.pclk_hz                       = 20 * 1000 * 1000;   /* 20 MHz */
    io_config.trans_queue_depth             = 10;
    io_config.on_color_trans_done           = Lcd_OnColorTransDone;
    io_config.lcd_cmd_bits                  = 8;
    io_config.lcd_param_bits                = 8;
    /* No quad_mode — ST7789V uses standard 4-wire SPI */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) lcd_host,
                                              &io_config, &io_handle));

    /* ---- ST7789V panel ---- */
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num             = LCD_RST_PIN;
    panel_config.rgb_ele_order              = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel             = LCD_BIT_PER_PIXEL;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

/* ========================================================================
 * LVGL Display Callbacks
 * ======================================================================== */

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    /* RGB565 byte swap for LVGL compatibility */
    lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));

    /* No x-offset needed for ST7789V (unlike SH8601 which needed +0x06) */
    esp_lcd_panel_draw_bitmap(panel,
                               area->x1, area->y1,
                               area->x2 + 1, area->y2 + 1,
                               color_p);
}

static void lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);

    /* Round to even boundaries for 2-byte alignment */
    uint16_t x1 = area->x1, x2 = area->x2;
    uint16_t y1 = area->y1, y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static void lvgl_wait_cb(lv_display_t *disp)
{
    xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
}

/* ========================================================================
 * LVGL Touch Input Callback (GT911 via touch_bsp)
 * ======================================================================== */

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *indevData)
{
    uint16_t x = 0, y = 0;

    if (g_touch_pnl && g_touch_pnl->GetCoords(&x, &y)) {
        /* GT911 reports coordinates matching display orientation on Leisound V1.
         * No coordinate flipping needed (unlike AMOLED reference which inverted).
         * Clamp to screen boundaries. */
        indevData->point.x = x;
        indevData->point.y = y;

        if (indevData->point.x >= LCD_H_RES)
            indevData->point.x = LCD_H_RES - 1;
        if (indevData->point.y >= LCD_V_RES)
            indevData->point.y = LCD_V_RES - 1;

        indevData->state = LV_INDEV_STATE_PRESSED;
    } else {
        indevData->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ========================================================================
 * LVGL Tick Timer
 * ======================================================================== */

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* ========================================================================
 * LVGL Locking (thread-safe access from multiple tasks)
 * ======================================================================== */

bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "Lvgl_PortInit must be called first");
    const TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
    assert(lvgl_mux && "Lvgl_PortInit must be called first");
    xSemaphoreGive(lvgl_mux);
}

/* ========================================================================
 * LVGL Port Task
 * ======================================================================== */

static void lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/* ========================================================================
 * Backlight Control
 *
 * ST7789V does not have a dedicated backlight command pin on this board.
 * Backlight is typically controlled via a separate GPIO PWM or is always-on.
 * For now, this is a no-op placeholder.
 * ======================================================================== */

void Lcd_SetBacklight(uint8_t brig)
{
    /* TODO: implement PWM backlight control if a backlight GPIO is available */
    (void)brig;
}

/* ========================================================================
 * Main Initialization
 * ======================================================================== */

void Lvgl_PortInit(void)
{
    /* Create synchronization objects */
    flush_done_semaphore = xSemaphoreCreateBinary();
    assert(flush_done_semaphore);
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    /* Initialize the LCD panel */
    Lvgl_LcdPanelInit(LCD_HOST);

    /* Initialize LVGL library */
    lv_init();

    /* Create display */
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_flush_wait_cb(disp, lvgl_wait_cb);
    lv_display_set_user_data(disp, panel_handle);
    lv_display_add_event_cb(disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    /* Allocate dual DMA-capable draw buffers (partial render mode) */
    uint8_t *buffer1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
    uint8_t *buffer2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
    assert(buffer1);
    assert(buffer2);
    lv_display_set_buffers(disp, buffer1, buffer2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Create touch input device */
    lv_indev_t *touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, lvgl_touch_cb);

    /* Start LVGL tick timer (2ms period) */
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback                = &increase_lvgl_tick;
    lvgl_tick_timer_args.name                    = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer           = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* Create LVGL port task */
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
}
