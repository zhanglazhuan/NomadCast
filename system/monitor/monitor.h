#ifndef MONITOR_H
#define MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── 采样 / 告警阈值配置 ─────────────────────────────────────────────────── */
#define MONITOR_SAMPLE_INTERVAL_SEC        60        /* 采样间隔(秒) */
#define MONITOR_DRAM_USED_PCT_THRESHOLD    80        /* 内部 DRAM 使用率告警阈值(%) */
#define MONITOR_PSRAM_USED_PCT_THRESHOLD   80        /* PSRAM 使用率告警阈值(%) */
#define MONITOR_DRAM_MIN_FREE_THRESHOLD    (32*1024) /* 内部 DRAM 剩余告警阈值(字节) */
#define MONITOR_ALERT_DURATION_SEC         30        /* 超阈持续该时长才记告警日志(秒) */

/** 启动运行时监控:按 MONITOR_SAMPLE_INTERVAL_SEC 周期打印指标并做阈值告警。 */
void monitor_init(void);

/** 立即打印一次指标并检测阈值(可手动触发)。 */
void monitor_report(void);

#ifdef __cplusplus
}
#endif

#endif
