#include "display_controller.h"

typedef struct {
    esp_lcd_panel_handle_t panel;
    gpio_num_t backlight_pin;
    bool awake;
} display_controller_ctx_t;

static display_controller_ctx_t s_ctx = {
    .backlight_pin = GPIO_NUM_NC,
};

static esp_err_t set_backlight(bool on)
{
    if (s_ctx.backlight_pin == GPIO_NUM_NC) return ESP_OK;
    return gpio_set_level(s_ctx.backlight_pin, on);
}

esp_err_t display_controller_set_backlight_pin(gpio_num_t pin)
{
    if (pin != GPIO_NUM_NC) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return ESP_ERR_INVALID_ARG;
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        esp_err_t err = gpio_config(&config);
        if (err != ESP_OK) return err;
        err = gpio_set_level(pin, s_ctx.awake);
        if (err != ESP_OK) return err;
    }
    s_ctx.backlight_pin = pin;
    return ESP_OK;
}

esp_err_t display_controller_init(esp_lcd_panel_handle_t panel, gpio_num_t backlight_pin)
{
    if (!panel) return ESP_ERR_INVALID_ARG;
    if (s_ctx.panel) return ESP_ERR_INVALID_STATE;
    esp_err_t err = display_controller_set_backlight_pin(backlight_pin);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_disp_on_off(panel, false);
    if (err != ESP_OK) return err;
    s_ctx.panel = panel;
    return ESP_OK;
}

esp_err_t display_controller_wake(void)
{
    if (!s_ctx.panel) return ESP_ERR_INVALID_STATE;
    if (s_ctx.awake) return ESP_OK;
    esp_err_t err = esp_lcd_panel_disp_on_off(s_ctx.panel, true);
    if (err != ESP_OK) return err;
    err = set_backlight(true);
    if (err != ESP_OK) {
        esp_lcd_panel_disp_on_off(s_ctx.panel, false);
        return err;
    }
    s_ctx.awake = true;
    return ESP_OK;
}

esp_err_t display_controller_sleep(void)
{
    if (!s_ctx.panel) return ESP_ERR_INVALID_STATE;
    if (!s_ctx.awake) return ESP_OK;
    esp_err_t err = set_backlight(false);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_disp_on_off(s_ctx.panel, false);
    if (err != ESP_OK) {
        set_backlight(true);
        return err;
    }
    s_ctx.awake = false;
    return ESP_OK;
}

bool display_controller_is_awake(void)
{
    return s_ctx.awake;
}
