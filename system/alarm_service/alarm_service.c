/*
 * NomadCast — Alarm Service
 *
 * Global alarm runtime: NVS-persisted alarms (once / daily / weekdays),
 * minute-granularity firing, snooze, deep-sleep RTC timer wakeup, ringtone,
 * and the snooze/dismiss popup. Runs independent of the alarm app UI — fires
 * even when the app is closed and the screen is off, and can wake the device
 * out of deep sleep.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"

#include "lvgl.h"

#include "alarm_service.h"
#include "alarm_tone.h"
#include "flash_store.h"
#include "sleep_monitor.h"
#include "audio_player.h"
#include "lv_bottom_sheet.h"
#include "lv_toast.h"
#include "lang.h"

extern const lv_font_t *g_cjk_font;

static const char *TAG = "alarm";

#define ALARM_NS  "alarm"
#define ALARM_KEY "list"

#define SNOOZE_MINUTES 5

static alarm_entry_t s_alarms[ALARM_MAX];
static int    s_count = 0;
static time_t s_next_fire = 0;       /* epoch of next real alarm, 0 = none */
static int    s_next_fire_index = -1;/* which alarm s_next_fire refers to */
static time_t s_snooze_until = 0;    /* snooze deadline, 0 = no snooze pending */
static int    s_ringing_index = -1;  /* alarm currently ringing (for snooze) */

/* Survives deep sleep — the alarm index armed before shutdown. RTC_DATA_ATTR is
 * initialized on cold boot and retained across deep-sleep wakes. */
static RTC_DATA_ATTR int32_t s_pending_index = -1;

/* Dismiss popup (global overlay, shown regardless of running app). */
static lv_bottom_sheet_t *s_sheet = NULL;

/* ── Persistence: "HHMM<en><mode><weekdays>;…" ────────────────────────────
 * mode '0' once / '1' daily / '2' weekdays; weekdays 2 hex chars (bit0=Sun).
 * Older entries of just "HHMM<en>" are read as daily. */

static uint8_t hex2(const char *s)
{
    uint8_t v = 0;
    for (int k = 0; k < 2; k++) {
        char c = s[k];
        v = (uint8_t)(v << 4);
        if (c >= '0' && c <= '9')       v = (uint8_t)(v | (c - '0'));
        else if (c >= 'a' && c <= 'f')  v = (uint8_t)(v | (c - 'a' + 10));
        else if (c >= 'A' && c <= 'F')  v = (uint8_t)(v | (c - 'A' + 10));
    }
    return v;
}

static void alarm_service_load(void)
{
    char buf[ALARM_MAX * 10 + 1];
    int len = flash_get_str(ALARM_NS, ALARM_KEY, buf, sizeof(buf), "");
    s_count = 0;
    if (len <= 0) return;

    char *p = buf;
    while (s_count < ALARM_MAX && *p) {
        char *sep = strchr(p, ';');
        if (sep) *sep = '\0';
        int elen = (int)strlen(p);

        if (elen >= 5) {
            int h = (p[0] - '0') * 10 + (p[1] - '0');
            int m = (p[2] - '0') * 10 + (p[3] - '0');
            bool en = (p[4] == '1');
            uint8_t repeat = ALARM_REPEAT_DAILY;   /* legacy default */
            uint8_t wd = 0;
            if (elen >= 6 && p[5] >= '0' && p[5] <= '2') {
                repeat = (uint8_t)(p[5] - '0');
            }
            if (elen >= 8) {
                wd = hex2(&p[6]);
            }
            if (h >= 0 && h < 24 && m >= 0 && m < 60) {
                s_alarms[s_count].hour = (uint8_t)h;
                s_alarms[s_count].minute = (uint8_t)m;
                s_alarms[s_count].enabled = en;
                s_alarms[s_count].repeat = repeat;
                s_alarms[s_count].weekdays = wd;
                s_count++;
            }
        }

        if (!sep) break;
        p = sep + 1;
    }
}

static void alarm_service_save(void)
{
    char buf[ALARM_MAX * 10 + 1];
    size_t off = 0;
    for (int i = 0; i < s_count; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%02d%02d%c%c%02X",
                                i ? ";" : "",
                                s_alarms[i].hour, s_alarms[i].minute,
                                s_alarms[i].enabled ? '1' : '0',
                                '0' + s_alarms[i].repeat,
                                s_alarms[i].weekdays);
    }
    flash_set_str(ALARM_NS, ALARM_KEY, buf);
}

/* ── Next fire epoch ────────────────────────────────────────────────────── */

static time_t compute_entry_next(const alarm_entry_t *a, time_t now,
                                 const struct tm *now_tm)
{
    struct tm t;

    switch (a->repeat) {
    case ALARM_REPEAT_ONCE:
        t = *now_tm;
        t.tm_hour = a->hour; t.tm_min = a->minute; t.tm_sec = 0; t.tm_isdst = -1;
        {
            time_t e = mktime(&t);
            return (e > now) ? e : 0;   /* already passed today → no future fire */
        }

    case ALARM_REPEAT_DAILY:
        t = *now_tm;
        t.tm_hour = a->hour; t.tm_min = a->minute; t.tm_sec = 0; t.tm_isdst = -1;
        {
            time_t e = mktime(&t);
            if (e <= now) e += 24 * 3600;
            return e;
        }

    case ALARM_REPEAT_WEEKDAYS:
        if (a->weekdays == 0) return 0;
        for (int d = 0; d < 7; d++) {
            t = *now_tm;
            t.tm_hour = a->hour; t.tm_min = a->minute; t.tm_sec = 0;
            t.tm_isdst = -1;
            t.tm_mday += d;
            time_t e = mktime(&t);       /* mktime normalizes → t.tm_wday correct */
            int wd = t.tm_wday;
            if ((a->weekdays & (1 << wd)) && e > now) return e;
        }
        return 0;
    }
    return 0;
}

static time_t compute_next_fire(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    time_t best = 0;
    int best_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (!s_alarms[i].enabled) continue;
        time_t e = compute_entry_next(&s_alarms[i], now, &tmv);
        if (e == 0) continue;
        if (best == 0 || e < best) { best = e; best_idx = i; }
    }
    s_next_fire_index = best_idx;
    return best;
}

/* ── Dismiss / snooze popup ─────────────────────────────────────────────── */

static void dismiss_btn_cb(lv_event_t *e)
{
    (void)e;
    alarm_service_dismiss();
}

static void snooze_btn_cb(lv_event_t *e)
{
    (void)e;
    alarm_service_snooze();
}

static void sheet_on_delete(lv_event_t *e)
{
    (void)e;
    s_sheet = NULL;
    alarm_tone_stop();   /* stop ringing however the sheet was dismissed */
}

static lv_obj_t *popup_button(lv_obj_t *parent, const char *text,
                              lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 44);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_set_style_text_font(lbl, g_cjk_font ? g_cjk_font : &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

static void alarm_service_show_popup(int h, int m)
{
    if (s_sheet) return;

    s_sheet = lv_bottom_sheet_create(lv_layer_top());
    if (!s_sheet) return;
    lv_obj_add_event_cb(s_sheet->overlay, sheet_on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *cont = lv_bottom_sheet_get_content(s_sheet);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_row(cont, 16, 0);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, tr(STR_ALARM_RINGING));
    lv_obj_set_style_text_font(title, g_cjk_font ? g_cjk_font : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x666666), 0);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *time_lbl = lv_label_create(cont);
    lv_label_set_text_fmt(time_lbl, "%02d:%02d", h, m);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x1976D2), 0);
    lv_obj_set_width(time_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *snooze = popup_button(row, tr(STR_ALARM_SNOOZE),
                                    lv_color_hex(0xEEEEEE), lv_color_hex(0x333333));
    lv_obj_add_event_cb(snooze, snooze_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dismiss = popup_button(row, tr(STR_ALARM_DISMISS),
                                     lv_color_hex(0x1976D2), lv_color_white());
    lv_obj_add_event_cb(dismiss, dismiss_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Ring / trigger ─────────────────────────────────────────────────────── */

static void alarm_service_ring(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    alarm_entry_t *a = &s_alarms[idx];

    ESP_LOGI(TAG, "RINGING %02d:%02d", a->hour, a->minute);

    if (sleep_monitor_is_sleeping()) sleep_monitor_wake();
    if (audio_player_is_active()) audio_player_stop();

    alarm_tone_start();
    alarm_service_show_popup(a->hour, a->minute);
    s_ringing_index = idx;
}

static void alarm_service_trigger(int idx)
{
    alarm_service_ring(idx);

    if (idx >= 0 && idx < s_count && s_alarms[idx].repeat == ALARM_REPEAT_ONCE) {
        s_alarms[idx].enabled = false;
        alarm_service_save();
    }
    s_next_fire = compute_next_fire();
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void alarm_service_init(void)
{
    alarm_service_load();
    s_next_fire = compute_next_fire();

    /* Timer wakeup from deep sleep → the armed alarm is due now. */
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
        s_pending_index >= 0) {
        int idx = (int)s_pending_index;
        s_pending_index = -1;
        ESP_LOGI(TAG, "Timer wakeup — firing armed alarm idx %d", idx);
        if (idx >= 0 && idx < s_count) {
            alarm_service_trigger(idx);
        } else {
            s_next_fire = compute_next_fire();
        }
    } else {
        ESP_LOGI(TAG, "ready (%d alarms, next fire in %ld s)",
                 s_count, s_next_fire ? (long)(s_next_fire - time(NULL)) : -1L);
    }
}

int alarm_service_count(void)
{
    return s_count;
}

const alarm_entry_t *alarm_service_get(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_alarms[i];
}

static void recompute(void)
{
    alarm_service_save();
    s_next_fire = compute_next_fire();
}

void alarm_service_add(int h, int m, bool en, uint8_t repeat, uint8_t weekdays)
{
    if (s_count >= ALARM_MAX) return;
    s_alarms[s_count].hour = (uint8_t)h;
    s_alarms[s_count].minute = (uint8_t)m;
    s_alarms[s_count].enabled = en;
    s_alarms[s_count].repeat = repeat;
    s_alarms[s_count].weekdays = weekdays;
    s_count++;
    recompute();
}

void alarm_service_set(int i, int h, int m, bool en, uint8_t repeat, uint8_t weekdays)
{
    if (i < 0 || i >= s_count) return;
    s_alarms[i].hour = (uint8_t)h;
    s_alarms[i].minute = (uint8_t)m;
    s_alarms[i].enabled = en;
    s_alarms[i].repeat = repeat;
    s_alarms[i].weekdays = weekdays;
    recompute();
}

void alarm_service_remove(int i)
{
    if (i < 0 || i >= s_count) return;
    for (int j = i; j < s_count - 1; j++) s_alarms[j] = s_alarms[j + 1];
    s_count--;
    recompute();
}

void alarm_service_reset(void)
{
    s_count = 0;
    s_next_fire = 0;
    s_next_fire_index = -1;
    s_snooze_until = 0;
    s_pending_index = -1;
    flash_erase_ns(ALARM_NS);
}

void alarm_service_process(void)
{
    time_t now = time(NULL);

    /* Snooze pending → wait for the snooze deadline, not the real schedule. */
    if (s_snooze_until != 0) {
        if (now < s_snooze_until) return;
        s_snooze_until = 0;
        alarm_service_ring(s_ringing_index);
        return;
    }

    if (s_next_fire == 0 || now < s_next_fire) return;

    int idx = s_next_fire_index;
    if (idx < 0 || idx >= s_count || !s_alarms[idx].enabled) {
        s_next_fire = compute_next_fire();
        return;
    }

    alarm_service_trigger(idx);
}

void alarm_service_prepare_sleep(void)
{
    s_pending_index = -1;

    time_t now = time(NULL);
    time_t target = 0;
    int idx = -1;

    if (s_snooze_until != 0 && s_snooze_until > now) {
        target = s_snooze_until;
        idx = s_ringing_index;
    } else if (s_next_fire > now) {
        target = s_next_fire;
        idx = s_next_fire_index;
    }

    if (target == 0 || idx < 0) return;

    esp_sleep_enable_timer_wakeup((uint64_t)(target - now) * 1000000ULL);
    s_pending_index = idx;

    ESP_LOGI(TAG, "armed deep-sleep wake in %ld s (alarm idx %d)",
             (long)(target - now), idx);
}

void alarm_service_dismiss(void)
{
    alarm_tone_stop();
    if (s_sheet) {
        lv_bottom_sheet_close(s_sheet);
        s_sheet = NULL;
    }
}

void alarm_service_snooze(void)
{
    alarm_tone_stop();
    if (s_sheet) {
        lv_bottom_sheet_close(s_sheet);
        s_sheet = NULL;
    }
    s_snooze_until = time(NULL) + SNOOZE_MINUTES * 60;
    lv_toast_show(tr(STR_ALARM_SNOOZE_TOAST), 3000);
    ESP_LOGI(TAG, "snoozed for %d min", SNOOZE_MINUTES);
}

/* ── Localized weekday / repeat helpers ─────────────────────────────────── */

static const char *const wd_short_zh[7] = {"日", "一", "二", "三", "四", "五", "六"};
static const char *const wd_short_en[7] = {"S", "M", "T", "W", "T", "F", "S"};
static const char *const wd_full_zh[7]  = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
static const char *const wd_full_en[7]  = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

const char *alarm_wd_short(int wd)
{
    if (wd < 0 || wd > 6) return "";
    return (lang_get() == LANG_ZH_CN) ? wd_short_zh[wd] : wd_short_en[wd];
}

void alarm_repeat_summary(const alarm_entry_t *a, char *buf, size_t n)
{
    if (!a || n == 0) return;
    buf[0] = '\0';

    switch (a->repeat) {
    case ALARM_REPEAT_ONCE:
        snprintf(buf, n, "%s", tr(STR_ALARM_REPEAT_ONCE));
        break;
    case ALARM_REPEAT_DAILY:
        snprintf(buf, n, "%s", tr(STR_ALARM_REPEAT_DAILY));
        break;
    case ALARM_REPEAT_WEEKDAYS: {
        if (a->weekdays == 0) {
            snprintf(buf, n, "%s", tr(STR_ALARM_REPEAT_WEEKDAYS));
            break;
        }
        const char *const *names = (lang_get() == LANG_ZH_CN) ? wd_full_zh : wd_full_en;
        size_t off = 0;
        for (int wd = 0; wd < 7; wd++) {
            if (!(a->weekdays & (1 << wd))) continue;
            off += (size_t)snprintf(buf + off, n > off ? n - off : 0,
                                    "%s%s", off ? " " : "", names[wd]);
        }
        break;
    }
    default:
        break;
    }
}
