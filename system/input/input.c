/*
 * NomadCast — Key Input (espressif/button v4.x)
 */

#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "input.h"
#include "leisound_v1.h"

static const char *TAG = "input";

#define LONG_PRESS_MS   800

static SemaphoreHandle_t s_mutex;

static input_callback_t g_input_cb;
static void            *g_user_data;

/* Multi-subscriber support */
static input_callback_t g_subs[INPUT_MAX_CALLBACKS];
static void            *g_subs_data[INPUT_MAX_CALLBACKS];
static int              g_subs_count;

/* Button handler: gpio passed via usr_data (called from button task) */
static void btn_handler(void *handle, void *gpio_ptr)
{
    int gpio = (int)(intptr_t)gpio_ptr;
    button_event_t evt = iot_button_get_event(handle);
    input_event_t event;

    switch (gpio) {
    case LEISOUND_PIN_KEY_VOL_DOWN:
        if (evt == BUTTON_SINGLE_CLICK)      event = INPUT_EVENT_VOL_DOWN;
        else if (evt == BUTTON_LONG_PRESS_START) event = INPUT_EVENT_PREV_TRACK;
        else return;
        break;
    case LEISOUND_PIN_KEY_VOL_UP:
        if (evt == BUTTON_SINGLE_CLICK)      event = INPUT_EVENT_VOL_UP;
        else if (evt == BUTTON_LONG_PRESS_START) event = INPUT_EVENT_NEXT_TRACK;
        else return;
        break;
    case LEISOUND_PIN_KEY_STOP:
        if (evt == BUTTON_SINGLE_CLICK)      event = INPUT_EVENT_PLAY_PAUSE;
        else return;
        break;
    case LEISOUND_PIN_KEY_POWER:
        if (evt == BUTTON_SINGLE_CLICK)      event = INPUT_EVENT_SCREEN_TOGGLE;
        else if (evt == BUTTON_LONG_PRESS_START) event = INPUT_EVENT_POWER_OFF;
        else return;
        break;
    default: return;
    }

    /* Snapshot subscriber list under lock, then dispatch outside lock
     * to avoid deadlock if a callback calls input_subscribe/unsubscribe. */
    input_callback_t cb_snap[INPUT_MAX_CALLBACKS];
    void            *data_snap[INPUT_MAX_CALLBACKS];
    int              count;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = g_subs_count;
    memcpy(cb_snap,   g_subs,      count * sizeof(input_callback_t));
    memcpy(data_snap, g_subs_data, count * sizeof(void *));
    xSemaphoreGive(s_mutex);

    /* Dispatch to legacy single callback (backward compat) */
    if (g_input_cb) g_input_cb(event, g_user_data);

    for (int i = 0; i < count; i++) {
        if (cb_snap[i]) cb_snap[i](event, data_snap[i]);
    }
}

static void create_button(int gpio)
{
    button_config_t btn_cfg = { .long_press_time = LONG_PRESS_MS };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = 1,
    };
    button_handle_t handle;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "GPIO%d fail", gpio); return; }
    iot_button_register_cb(handle, BUTTON_SINGLE_CLICK,     NULL, btn_handler, (void *)(intptr_t)gpio);
    iot_button_register_cb(handle, BUTTON_LONG_PRESS_START, NULL, btn_handler, (void *)(intptr_t)gpio);
    ESP_LOGI(TAG, "Btn GPIO%d", gpio);
}

void input_init(input_callback_t cb, void *user_data)
{
    s_mutex = xSemaphoreCreateMutex();
    g_input_cb  = cb;
    g_user_data = user_data;
    g_subs_count = 0;
    create_button(LEISOUND_PIN_KEY_VOL_DOWN);
    create_button(LEISOUND_PIN_KEY_VOL_UP);
    create_button(LEISOUND_PIN_KEY_STOP);
    create_button(LEISOUND_PIN_KEY_POWER);
    ESP_LOGI(TAG, "All keys ready");
}

void input_subscribe(input_callback_t cb, void *user_data)
{
    if (!cb) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (g_subs_count >= INPUT_MAX_CALLBACKS) { xSemaphoreGive(s_mutex); return; }
    /* Avoid duplicate */
    for (int i = 0; i < g_subs_count; i++) {
        if (g_subs[i] == cb) { xSemaphoreGive(s_mutex); return; }
    }
    g_subs[g_subs_count]      = cb;
    g_subs_data[g_subs_count] = user_data;
    g_subs_count++;
    xSemaphoreGive(s_mutex);
}

void input_unsubscribe(input_callback_t cb)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < g_subs_count; i++) {
        if (g_subs[i] == cb) {
            /* Shift remaining entries down */
            for (int j = i; j < g_subs_count - 1; j++) {
                g_subs[j]      = g_subs[j + 1];
                g_subs_data[j] = g_subs_data[j + 1];
            }
            g_subs_count--;
            g_subs[g_subs_count]      = NULL;
            g_subs_data[g_subs_count] = NULL;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}
