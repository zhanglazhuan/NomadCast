/*
 * NomadCast — Alarm Service (public API)
 *
 * Global alarm runtime independent of the alarm app UI. Persists a list of
 * alarms in NVS, fires at minute granularity (even while the screen is off),
 * arms a deep-sleep RTC timer so an alarm can wake the device, and drives the
 * ringtone + snooze/dismiss popup. Each alarm repeats once, daily, or on a
 * chosen set of weekdays.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX 8

typedef enum {
    ALARM_REPEAT_ONCE = 0,      /* ring once, then auto-disable */
    ALARM_REPEAT_DAILY = 1,     /* every day */
    ALARM_REPEAT_WEEKDAYS = 2,  /* on the weekdays in `weekdays` */
} alarm_repeat_t;

typedef struct {
    uint8_t hour;      /* 0-23 */
    uint8_t minute;    /* 0-59 */
    bool    enabled;
    uint8_t repeat;    /* alarm_repeat_t */
    uint8_t weekdays;  /* bitmask: bit0=Sunday … bit6=Saturday (tm_wday) */
} alarm_entry_t;

/* Boot: load alarms, recompute next fire, and (after a timer-wake) ring. */
void alarm_service_init(void);

/* Main-loop poll: fire when the next enabled alarm is due. Cheap — call every
 * iteration of the LVGL loop. */
void alarm_service_process(void);

/* Deep-sleep arm: schedule a RTC timer wakeup for the next alarm. Call before
 * esp_deep_sleep_start(). */
void alarm_service_prepare_sleep(void);

/* Stop ringing and close the dismiss popup (keeps the alarm enabled). */
void alarm_service_dismiss(void);

/* Stop ringing, close the popup, and re-arm to ring again in a few minutes. */
void alarm_service_snooze(void);

/* Factory reset: clear the alarm list in RAM and NVS. */
void alarm_service_reset(void);

int  alarm_service_count(void);
const alarm_entry_t *alarm_service_get(int i);

void alarm_service_add(int h, int m, bool en, uint8_t repeat, uint8_t weekdays);
void alarm_service_set(int i, int h, int m, bool en, uint8_t repeat, uint8_t weekdays);
void alarm_service_remove(int i);

/* Localized short weekday label for a toggle button (wd 0=Sun … 6=Sat). */
const char *alarm_wd_short(int wd);

/* Localized one-line repeat description ("Daily", "Once", "Mon Tue Wed …"). */
void alarm_repeat_summary(const alarm_entry_t *a, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
