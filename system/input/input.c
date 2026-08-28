/*
 * NomadCast — Key Input (espressif/button v4.x)
 */

#include "esp_log.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "input.h"
#include "leisound_v1.h"

static const char *TAG = "input";

#define LONG_PRESS_MS        800
/* 开机免疫期：开机后此时间内忽略短按息屏，吞掉长按开机松手时的误触 */
#define BOOT_SCREEN_TOGGLE_IMMUNITY_MS  10000

typedef struct {
    SemaphoreHandle_t mutex;
    input_callback_t callback;
    void *user_data;
    input_callback_t subscribers[INPUT_MAX_CALLBACKS];
    void *subscriber_data[INPUT_MAX_CALLBACKS];
    int subscriber_count;
} input_state_t;

static input_state_t s_input;

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
        {
            /* 开机连按/粘滞保护：开机时长按残留的高电平会被 iot_button 误判成
             * 新的长按/短按。开机后必须等到第一次松手，才恢复正常按键逻辑。 */
            static bool s_waiting_for_first_release = true;
            uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

            if (uptime_ms < BOOT_SCREEN_TOGGLE_IMMUNITY_MS) {
                if (s_waiting_for_first_release) {
                    /* 免疫期 + 还没松过手：吞掉所有事件，直到检测到释放 */
                    if (evt == BUTTON_PRESS_UP || evt == BUTTON_LONG_PRESS_UP) {
                        s_waiting_for_first_release = false;
                    }
                    return;
                }
                /* 已松过手，但免疫期内短按仍忽略（防松手误触息屏） */
                if (evt == BUTTON_SINGLE_CLICK) {
                    return;
                }
            } else {
                /* 超过免疫期，解除锁定 */
                s_waiting_for_first_release = false;
            }

            /* 正常电源键逻辑 */
            if (evt == BUTTON_LONG_PRESS_START) {
                event = INPUT_EVENT_POWER_OFF;
            } else if (evt == BUTTON_SINGLE_CLICK) {
                event = INPUT_EVENT_SCREEN_TOGGLE;
            } else {
                return;
            }
        }
        break;
    default: return;
    }

    /* Snapshot subscriber list under lock, then dispatch outside lock
     * to avoid deadlock if a callback calls input_subscribe/unsubscribe. */
    input_callback_t cb_snap[INPUT_MAX_CALLBACKS];
    void            *data_snap[INPUT_MAX_CALLBACKS];
    int              count;

    xSemaphoreTake(s_input.mutex, portMAX_DELAY);
    count = s_input.subscriber_count;
    memcpy(cb_snap,   s_input.subscribers,      count * sizeof(input_callback_t));
    memcpy(data_snap, s_input.subscriber_data, count * sizeof(void *));
    xSemaphoreGive(s_input.mutex);

    /* Dispatch to legacy single callback (backward compat) */
    if (s_input.callback) s_input.callback(event, s_input.user_data);

    for (int i = 0; i < count; i++) {
        if (cb_snap[i]) cb_snap[i](event, data_snap[i]);
    }
}

static void create_button(int gpio, uint16_t long_press_ms)
{
    button_config_t btn_cfg = { .long_press_time = long_press_ms };
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
    s_input.mutex = xSemaphoreCreateMutex();
    s_input.callback  = cb;
    s_input.user_data = user_data;
    s_input.subscriber_count = 0;
    create_button(LEISOUND_PIN_KEY_VOL_DOWN, LONG_PRESS_MS);
    create_button(LEISOUND_PIN_KEY_VOL_UP,   LONG_PRESS_MS);
    create_button(LEISOUND_PIN_KEY_STOP,     LONG_PRESS_MS);
    create_button(LEISOUND_PIN_KEY_POWER,    POWER_OFF_LONG_PRESS_MS);
    ESP_LOGI(TAG, "All keys ready");
}

void input_subscribe(input_callback_t cb, void *user_data)
{
    if (!cb) return;
    xSemaphoreTake(s_input.mutex, portMAX_DELAY);
    if (s_input.subscriber_count >= INPUT_MAX_CALLBACKS) { xSemaphoreGive(s_input.mutex); return; }
    /* Avoid duplicate */
    for (int i = 0; i < s_input.subscriber_count; i++) {
        if (s_input.subscribers[i] == cb) { xSemaphoreGive(s_input.mutex); return; }
    }
    s_input.subscribers[s_input.subscriber_count]      = cb;
    s_input.subscriber_data[s_input.subscriber_count] = user_data;
    s_input.subscriber_count++;
    xSemaphoreGive(s_input.mutex);
}

void input_unsubscribe(input_callback_t cb)
{
    xSemaphoreTake(s_input.mutex, portMAX_DELAY);
    for (int i = 0; i < s_input.subscriber_count; i++) {
        if (s_input.subscribers[i] == cb) {
            /* Shift remaining entries down */
            for (int j = i; j < s_input.subscriber_count - 1; j++) {
                s_input.subscribers[j]      = s_input.subscribers[j + 1];
                s_input.subscriber_data[j] = s_input.subscriber_data[j + 1];
            }
            s_input.subscriber_count--;
            s_input.subscribers[s_input.subscriber_count]      = NULL;
            s_input.subscriber_data[s_input.subscriber_count] = NULL;
            break;
        }
    }
    xSemaphoreGive(s_input.mutex);
}
