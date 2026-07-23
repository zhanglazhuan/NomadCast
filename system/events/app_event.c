/**
 * @file app_event.c
 * @brief Simple event bus — fixed listener table.
 */
#include "app_event.h"
#include <stdio.h>

/* Must exceed the number of app_event_register() call sites (status bar, clock,
 * audio player, podcast controller, local page, download-task page, …). Some
 * register lazily when a page first opens, so a too-small cap silently DROPS the
 * later ones and their live-refresh stops working. */
#define MAX_LISTENERS 16

static app_event_cb_t s_listeners[MAX_LISTENERS];
static int s_listener_count = 0;

void app_event_register(app_event_cb_t cb)
{
    if (s_listener_count < MAX_LISTENERS) {
        s_listeners[s_listener_count++] = cb;
        printf("[app_event] register listener #%d (cb=%p)\n", s_listener_count, (void *)cb);
        fflush(stdout);
    } else {
        printf("[app_event] DROPPED listener (cb=%p) — MAX_LISTENERS=%d reached!\n",
               (void *)cb, MAX_LISTENERS);
        fflush(stdout);
    }
}

void app_event_fire(app_event_t event, const void *data)
{
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i]) {
            s_listeners[i](event, data);
        }
    }
}
