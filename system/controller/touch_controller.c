#include "touch_controller.h"
#include "cst836u.h"
#include "nomadcast.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "touch_controller";
typedef struct {
    cst836u_handle_t touch;
    lv_indev_t *indev;
    lv_point_t point;
    lv_indev_state_t state;
    int64_t last_error;
} touch_controller_ctx_t;

static touch_controller_ctx_t s_ctx = {
    .state = LV_INDEV_STATE_RELEASED,
};

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    cst836u_touch_t report;
    esp_err_t err = cst836u_read(s_ctx.touch, &report);
    if (err == ESP_OK) {
        s_ctx.state = LV_INDEV_STATE_RELEASED;
        /* Select an active point: the first packet slot may contain an UP. */
        for (unsigned i = 0; report.count > 0 && i < CST836U_MAX_POINTS; ++i) {
            if (!report.points[i].active) continue;
            int32_t x = report.points[i].x;
            int32_t y = report.points[i].y;
            lv_display_t *display = lv_indev_get_display(indev);
            int32_t width = lv_display_get_horizontal_resolution(display);
            int32_t height = lv_display_get_vertical_resolution(display);
            if (x >= width || y >= height) continue;
            s_ctx.point.x = x;
            s_ctx.point.y = y;
            s_ctx.state = LV_INDEV_STATE_PRESSED;
            break;
        }
    } else {
        /* A failed transfer is not a release report. */
        int64_t now = esp_timer_get_time();
        if (!s_ctx.last_error || now - s_ctx.last_error >= 1000000) {
            ESP_LOGW(TAG, "Touch read failed: %s", esp_err_to_name(err));
            s_ctx.last_error = now;
        }
    }
    data->state = s_ctx.state;
    data->point = s_ctx.point;
}

esp_err_t touch_controller_init(i2c_master_bus_handle_t bus, lv_display_t *display,
                                lv_indev_t **indev)
{
    if (!bus || !display || !indev) return ESP_ERR_INVALID_ARG;
    *indev = NULL;
    if (s_ctx.indev) return ESP_ERR_INVALID_STATE;
    cst836u_config_t config = CST836U_CONFIG_DEFAULT();
    config.int_gpio = NOMADCAST_PIN_TP_INT;
    /* LCD shares RST: never reset it after the display has been initialized. */
    esp_err_t err = cst836u_init(bus, &config, &s_ctx.touch);
    if (err != ESP_OK) return err;
    s_ctx.indev = lv_indev_create();
    if (!s_ctx.indev) {
        cst836u_deinit(s_ctx.touch);
        s_ctx.touch = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_ctx.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_ctx.indev, display);
    lv_indev_set_read_cb(s_ctx.indev, read_touch);
    lv_indev_set_scroll_limit(s_ctx.indev, 20);
    lv_timer_set_period(lv_indev_get_read_timer(s_ctx.indev), 20);
    *indev = s_ctx.indev;
    return ESP_OK;
}
