/* ST7789 settings verified on PCB V1.2 with debug/t_display. */
#include "nomadcast_display.h"
#include "nomadcast.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void nomadcast_display_power_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(NOMADCAST_PIN_LCD_POWER) | BIT64(NOMADCAST_PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_LCD_POWER, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_LCD_BL, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_lcd_panel_handle_t nomadcast_display_init(size_t max_transfer_bytes)
{
    spi_bus_config_t spi_cfg = {
        .mosi_io_num = NOMADCAST_PIN_SPI_MOSI, .miso_io_num = NOMADCAST_PIN_SPI_MISO,
        .sclk_io_num = NOMADCAST_PIN_SPI_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1,
        .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = max_transfer_bytes,
        .flags = 0, .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO, .intr_flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(NOMADCAST_LCD_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = NOMADCAST_PIN_LCD_CS, .dc_gpio_num = NOMADCAST_PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = (20 * 1000 * 1000), .trans_queue_depth = 10,
        .on_color_trans_done = NULL, .user_ctx = NULL,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .flags = {0},
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)NOMADCAST_LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .flags = {0}, .vendor_config = NULL,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    /* Display stays OFF until launcher renders — prevents boot garbage */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
    return panel;
}

