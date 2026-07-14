/*
 * NomadCast — Sleep / Idle Monitor
 */

#include "sleep_monitor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sleep";

/* ── State ────────────────────────────────────────────────────────────────── */

static esp_lcd_panel_handle_t s_panel       = NULL;
static lv_indev_t            *s_touch_indev = NULL;
static lv_timer_t            *s_idle_timer  = NULL;
static int                    s_timeout_min = 0;
static bool                   s_is_sleeping = false;
static uint64_t               s_last_activity_ms = 0;
static gpio_num_t             s_power_pin   = GPIO_NUM_NC;
static sleep_monitor_wake_cb_t s_wake_cb    = NULL;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void wake_from_sleep(void);
static void enter_sleep(void);

/* ── Activity tracking ────────────────────────────────────────────────────── */

static void on_touch_activity(lv_event_t *e)
{
    (void)e;
    sleep_monitor_notify_activity();
}

static void on_button_event(input_event_t event, void *user_data)
{
    (void)user_data;

    if (s_is_sleeping) {
        /* Only Power short-press wakes. Long-press (POWER_OFF) also wakes
         * so the user sees the shutdown happen. */
        if (event == INPUT_EVENT_SCREEN_TOGGLE || event == INPUT_EVENT_POWER_OFF) {
            wake_from_sleep();
        }
        return;
    }

    /* When awake: Power short-press → manual sleep; any button resets idle */
    if (event == INPUT_EVENT_SCREEN_TOGGLE) {
        enter_sleep();
        return;
    }
    sleep_monitor_notify_activity();
}

/* ── Sleep / Wake helpers ─────────────────────────────────────────────────── */

static void enter_sleep(void)
{
    if (s_is_sleeping) return;
    ESP_LOGI(TAG, "Manual sleep via power key");

    /* 1. Blank the display */
    esp_lcd_panel_disp_on_off(s_panel, false);

    /* 2. Disable touch */
    if (s_touch_indev) {
        lv_indev_enable(s_touch_indev, false);
    }

    /* 3. Keep peripheral power ON — SD card downloads must survive screen-off.
     *    Only blank display + disable touch.  The LCD backlight is the dominant
     *    power consumer; GPIO 46's savings are negligible. */
    (void)s_power_pin;

    s_is_sleeping = true;
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_is_sleeping) return;
    if (s_timeout_min <= 0) return;

    uint64_t now_ms = esp_timer_get_time() / 1000;
    uint64_t elapsed_ms = now_ms - s_last_activity_ms;
    uint64_t timeout_ms = (uint64_t)s_timeout_min * 60 * 1000;

    if (elapsed_ms >= timeout_ms) {
        enter_sleep();
    }
}

/* ── Sleep / Wake ─────────────────────────────────────────────────────────── */

static void wake_from_sleep(void)
{
    if (!s_is_sleeping) return;
    s_is_sleeping = false;
    ESP_LOGI(TAG, "Waking from sleep");

    /* Power was never cut — just turn display and touch back on */
    esp_lcd_panel_disp_on_off(s_panel, true);

    if (s_touch_indev) {
        lv_indev_enable(s_touch_indev, true);
    }

    s_last_activity_ms = esp_timer_get_time() / 1000;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void sleep_monitor_init(esp_lcd_panel_handle_t panel,
                        lv_indev_t             *touch_indev,
                        int                     timeout_min)
{
    s_panel       = panel;
    s_touch_indev = touch_indev;
    s_timeout_min = timeout_min;
    s_is_sleeping = false;
    s_last_activity_ms = esp_timer_get_time() / 1000;

    /* Hook touch indev for activity detection */
    if (touch_indev) {
        lv_indev_add_event_cb(touch_indev, on_touch_activity,
                              LV_EVENT_PRESSED, NULL);
    }

    /* Subscribe to physical keys */
    input_subscribe(on_button_event, NULL);

    /* Start the 1-second polling timer */
    s_idle_timer = lv_timer_create(idle_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_idle_timer, -1); /* infinite */

    ESP_LOGI(TAG, "Initialized (timeout=%d min)", timeout_min);
}

void sleep_monitor_set_timeout(int timeout_min)
{
    s_timeout_min = timeout_min;
    s_last_activity_ms = esp_timer_get_time() / 1000;

    /* If we were asleep but timeout is now set, wake up */
    if (s_is_sleeping && timeout_min > 0) {
        wake_from_sleep();
    }

    ESP_LOGI(TAG, "Timeout set to %d min", timeout_min);
}

int sleep_monitor_get_timeout(void)
{
    return s_timeout_min;
}

void sleep_monitor_notify_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

bool sleep_monitor_is_sleeping(void)
{
    return s_is_sleeping;
}

void sleep_monitor_set_power_pin(gpio_num_t pin)
{
    s_power_pin = pin;
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 1);  /* default: power ON */
    ESP_LOGI(TAG, "Power pin GPIO%d configured", pin);
}

void sleep_monitor_set_wake_callback(sleep_monitor_wake_cb_t cb)
{
    s_wake_cb = cb;
}
