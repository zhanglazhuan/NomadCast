/*
 * NomadCast — Key Input Module
 *
 * Wraps espressif/button to provide app-level key events.
 * Supports short-press and long-press detection on all physical keys.
 *
 * Pin mapping: board/leisound_v1.h
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Key event types
 * ======================================================================== */

typedef enum {
    INPUT_EVENT_VOL_DOWN,        /* Vol-  short press */
    INPUT_EVENT_VOL_UP,          /* Vol+  short press */
    INPUT_EVENT_PLAY_PAUSE,      /* Stop  short press */
    INPUT_EVENT_PREV_TRACK,      /* Vol-  long press */
    INPUT_EVENT_NEXT_TRACK,      /* Vol+  long press */
    INPUT_EVENT_SCREEN_TOGGLE,   /* PWR   short press */
    INPUT_EVENT_POWER_OFF,       /* PWR   long press */
} input_event_t;

/* ========================================================================
 * Callback type
 * ======================================================================== */

typedef void (*input_callback_t)(input_event_t event, void *user_data);

#define INPUT_MAX_CALLBACKS 4

/* 电源键长按阈值 (ms) */
#define POWER_OFF_LONG_PRESS_MS  2500
#define POWER_ON_LONG_PRESS_MS    800

/* ========================================================================
 * Public API
 * ======================================================================== */

/**
 * @brief Initialize all physical keys and subscribe the primary callback.
 *
 * Uses espressif/button component for debounce + short/long detection.
 * Power-off long-press threshold: 2500 ms.
 *
 * @param cb         Callback invoked on key events (from button task context).
 * @param user_data  Opaque pointer passed to callback.
 */
void input_init(input_callback_t cb, void *user_data);

/**
 * @brief Subscribe an additional callback to all key events.
 *
 * Up to INPUT_MAX_CALLBACKS subscribers (including the one from input_init).
 * Returns silently if the array is full or cb is NULL.
 */
void input_subscribe(input_callback_t cb, void *user_data);

/**
 * @brief Unsubscribe a previously registered callback.
 */
void input_unsubscribe(input_callback_t cb);

#ifdef __cplusplus
}
#endif
