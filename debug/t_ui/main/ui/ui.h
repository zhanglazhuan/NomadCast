/*
 * t_ui — UI Module: LVGL Button + Swipe Gesture
 *
 * Creates a centered button (click → log) and detects
 * left-swipe gestures (log) on the active screen.
 */

#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the LVGL UI on the active screen.
 *
 * Creates:
 *  - A centered button: 140×50 px, label "Click Me"
 *    Logs "Button clicked!" on LV_EVENT_CLICKED.
 *  - A swipe gesture handler on the screen:
 *    Logs "Left swipe detected" when horizontal drag < -50 px.
 *
 * Call after lvgl display and indev are registered.
 */
void ui_init(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
