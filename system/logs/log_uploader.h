/**
 * @file log_uploader.h
 * @brief Internal — low-priority WiFi log upload task.
 */
#ifndef LOG_UPLOADER_H
#define LOG_UPLOADER_H
#include <stdbool.h>

/** Start the uploader task. Called once by log_system_init(). */
void log_uploader_start(void);

/** Notify uploader that WiFi just connected — check for pending uploads. */
void log_uploader_on_wifi_connected(void);

void log_uploader_pause(bool paused);

#endif
