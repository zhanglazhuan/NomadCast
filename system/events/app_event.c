/**
 * @file app_event.c
 * @brief Simple event bus — fixed listener table.
 */
#include "app_event.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Must exceed the number of app_event_register() call sites (status bar, clock,
 * audio player, podcast controller, local page, download-task page, …). Some
 * register lazily when a page first opens, so a too-small cap silently DROPS the
 * later ones and their live-refresh stops working. */
#define MAX_LISTENERS 16

typedef struct {
    app_event_t event;
    union {
        app_event_clock_tick_t clock;
        app_event_battery_t battery;
    } payload;
    bool has_data;
} event_msg_t;

typedef struct {
    app_event_cb_t listeners[MAX_LISTENERS];
    int listener_count;
    QueueHandle_t event_queue;
} app_event_state_t;

static app_event_state_t s_events;

void app_event_register(app_event_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < s_events.listener_count; i++) if (s_events.listeners[i] == cb) return;
    if (s_events.listener_count < MAX_LISTENERS) {
        s_events.listeners[s_events.listener_count++] = cb;
        printf("[app_event] register listener #%d (cb=%p)\n", s_events.listener_count, (void *)cb);
        fflush(stdout);
    } else {
        printf("[app_event] DROPPED listener (cb=%p) — MAX_LISTENERS=%d reached!\n",
               (void *)cb, MAX_LISTENERS);
        fflush(stdout);
    }
}

void app_event_unregister(app_event_cb_t cb)
{
    for (int i = 0; i < s_events.listener_count; i++) {
        if (s_events.listeners[i] == cb) { s_events.listeners[i] = s_events.listeners[--s_events.listener_count]; return; }
    }
}

void app_event_fire(app_event_t event, const void *data)
{
    if (!s_events.event_queue) s_events.event_queue = xQueueCreate(16, sizeof(event_msg_t));
    if (!s_events.event_queue) return;
    event_msg_t msg = { .event = event, .has_data = data != NULL };
    if (data) {
        if (event == APP_EVENT_CLOCK_TICK) msg.payload.clock = *(const app_event_clock_tick_t *)data;
        else if (event == APP_EVENT_BATTERY_CHANGED) msg.payload.battery = *(const app_event_battery_t *)data;
    }
    (void)xQueueSend(s_events.event_queue, &msg, 0);
}

void app_event_process(void)
{
    event_msg_t msg;
    while (s_events.event_queue && xQueueReceive(s_events.event_queue, &msg, 0) == pdTRUE) {
        const void *data = NULL;
        if (msg.has_data) {
            if (msg.event == APP_EVENT_CLOCK_TICK) data = &msg.payload.clock;
            else if (msg.event == APP_EVENT_BATTERY_CHANGED) data = &msg.payload.battery;
        }
    for (int i = 0; i < s_events.listener_count; i++) {
        if (s_events.listeners[i]) {
            s_events.listeners[i](msg.event, data);
        }
    }
    }
}
