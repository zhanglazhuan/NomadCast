/* PCB V1.2 harness; driver source and header are compiled directly from refer/. */
#include <inttypes.h>
#include "cst836u.h"
#include "nomadcast.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "t_CST836U";
static void wait_ms(unsigned ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}
void app_main(void) {
    ESP_LOGI(TAG, "DIRECT refer/read-pico-reader-main/components/cst836u; PCB V1.2");
    ESP_LOGI(TAG, "SDA=%d SCL=%d RST=%d INT=%d", NOMADCAST_PIN_I2C_SDA,
             NOMADCAST_PIN_I2C_SCL, NOMADCAST_PIN_TP_RST, NOMADCAST_PIN_TP_INT);
    gpio_config_t power = {
        .pin_bit_mask = BIT64(NOMADCAST_PIN_AP_POWER) | BIT64(NOMADCAST_PIN_LCD_POWER) |
                        BIT64(NOMADCAST_PIN_LCD_BL) | BIT64(NOMADCAST_PIN_TP_RST),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&power));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_LCD_BL, 0));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_TP_RST, 0));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_AP_POWER, 1));
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_LCD_POWER, 1));
    wait_ms(100);
    ESP_ERROR_CHECK(gpio_set_level(NOMADCAST_PIN_TP_RST, 1));
    wait_ms(50);
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = NOMADCAST_PIN_I2C_SDA,
        .scl_io_num = NOMADCAST_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    cst836u_config_t config = CST836U_CONFIG_DEFAULT();
    config.int_gpio = NOMADCAST_PIN_TP_INT;
    config.rst_gpio = NOMADCAST_PIN_TP_RST;
    cst836u_handle_t touch = NULL;
    esp_err_t error = cst836u_init(bus, &config, &touch);
    ESP_LOGI(TAG, "Reference cst836u_init: %s", esp_err_to_name(error));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Reference init failed; no handle, point reads skipped");
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }
    cst836u_info_t info;
    ESP_ERROR_CHECK(cst836u_get_info(touch, &info));
    ESP_LOGI(TAG, "FW=%02x ID=%02x module=%02x project=%02x type=%04x",
             info.fw, info.id, info.module, info.project, info.type);
    uint32_t reads = 0, errors = 0, presses = 0, releases = 0;
    bool pressed = false;
    int64_t next_status = esp_timer_get_time() + 5000000;
    int64_t last_error = 0, last_point = 0;
    while (1) {
        cst836u_touch_t report;
        error = cst836u_read(touch, &report);
        int64_t now = esp_timer_get_time();
        if (error != ESP_OK) {
            ++errors;
            if (errors == 1 || now - last_error >= 1000000) {
                ESP_LOGW(TAG, "Reference read: %s (not release)", esp_err_to_name(error));
                last_error = now;
            }
        } else {
            ++reads;
            if (report.touched && (!pressed || now - last_point >= 100000)) {
                ESP_LOGI(TAG, "%s count=%u P1=%s %u,%u P2=%s %u,%u INT=%d",
                         pressed ? "MOVE" : "PRESS", report.count,
                         cst836u_event_name(report.points[0].event), report.points[0].x, report.points[0].y,
                         cst836u_event_name(report.points[1].event), report.points[1].x, report.points[1].y,
                         cst836u_int_level(touch));
                last_point = now;
            }
            if (report.touched && !pressed) ++presses;
            if (!report.touched && pressed) { ++releases; ESP_LOGI(TAG, "RELEASE"); }
            pressed = report.touched;
            if (reads == 1) ESP_LOG_BUFFER_HEX_LEVEL(TAG, report.raw, sizeof(report.raw), ESP_LOG_INFO);
        }
        if (now >= next_status) {
            ESP_LOGI(TAG, "STATUS reads=%"PRIu32" errors=%"PRIu32" presses=%"PRIu32" releases=%"PRIu32,
                     reads, errors, presses, releases);
            next_status = now + 5000000;
        }
        wait_ms(20);
    }
}
