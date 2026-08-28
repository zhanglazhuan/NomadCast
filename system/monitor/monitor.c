/**
 * @file monitor.c
 * @brief 运行时性能监控 — 定期打印堆(内部 DRAM + PSRAM)指标,持续超阈时记告警日志
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "monitor.h"

static const char *TAG = "monitor";

/* 超阈持续检测状态 */
typedef struct {
    int64_t over_start_us;
    bool alerted;
} monitor_state_t;

static monitor_state_t s_monitor;

/* 检测是否超阈,超阈持续 MONITOR_ALERT_DURATION_SEC 才记一次告警 */
static void monitor_check_alert(int drm_pct, int ps_pct, size_t drm_free)
{
    bool over = (drm_pct >= MONITOR_DRAM_USED_PCT_THRESHOLD) ||
                (ps_pct  >= MONITOR_PSRAM_USED_PCT_THRESHOLD) ||
                (drm_free <= MONITOR_DRAM_MIN_FREE_THRESHOLD);

    if (over) {
        if (s_monitor.over_start_us == 0) {
            s_monitor.over_start_us = esp_timer_get_time();
        }
        int64_t elapsed_us = esp_timer_get_time() - s_monitor.over_start_us;
        if (!s_monitor.alerted && elapsed_us >= (int64_t)MONITOR_ALERT_DURATION_SEC * 1000000LL) {
            s_monitor.alerted = true;
            ESP_LOGW(TAG, "RESOURCE ALERT: DRAM=%d%% PSRAM=%d%% DRAM_free=%u B "
                     "(over threshold for >=%d s)",
                     drm_pct, ps_pct, (unsigned)drm_free, MONITOR_ALERT_DURATION_SEC);
        }
    } else {
        s_monitor.over_start_us = 0;
        s_monitor.alerted = false;
    }
}

void monitor_report(void)
{
    /* 堆:内部 DRAM(稀缺,~333KB) + PSRAM(富余,8MB) */
    size_t drm_total   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t drm_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t drm_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t drm_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t ps_total    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_min      = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_largest  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    int drm_pct = drm_total ? (int)((drm_total - drm_free) * 100 / drm_total) : 0;
    int ps_pct  = ps_total  ? (int)((ps_total  - ps_free)  * 100 / ps_total)  : 0;

    ESP_LOGI(TAG, "DRAM : used=%u/%u B (%d%%)  min_free=%u  largest=%u",
             (unsigned)(drm_total - drm_free), (unsigned)drm_total, drm_pct,
             (unsigned)drm_min, (unsigned)drm_largest);
    ESP_LOGI(TAG, "PSRAM: used=%u/%u B (%d%%)  min_free=%u  largest=%u",
             (unsigned)(ps_total - ps_free), (unsigned)ps_total, ps_pct,
             (unsigned)ps_min, (unsigned)ps_largest);

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
    /* 任务栈高水位(vTaskList 格式:name/state/prio/stack-free(words)/task#) */
    static char taskbuf[1024];
    vTaskList(taskbuf);
    ESP_LOGI(TAG, "Task list (stack-free in words):\n%s", taskbuf);
#endif

    monitor_check_alert(drm_pct, ps_pct, drm_free);
}

static void monitor_timer_cb(void *arg)
{
    (void)arg;
    monitor_report();
}

void monitor_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = monitor_timer_cb,
        .arg = NULL,
        .name = "monitor",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, (uint64_t)MONITOR_SAMPLE_INTERVAL_SEC * 1000000ULL);
        ESP_LOGI(TAG, "monitor started (every %d s)", MONITOR_SAMPLE_INTERVAL_SEC);
    } else {
        ESP_LOGE(TAG, "monitor timer create failed");
    }
}
