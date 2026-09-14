#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "view_current.h"
#include "../view.h"
#include "../app.h"
#include "../model.h"
#include "../controller.h"
#include "lv_page.h"
#include "lv_home_indicator.h"
#include "lang.h"

extern WeatherApp g_weather_app;
extern const lv_font_t *g_cjk_font;

/* "YYYY-MM-DD" → localized weekday + "MM-DD". */
static void format_day(char *out, size_t n, const char *ymd) {
    struct tm t = {0};
    if (!ymd || sscanf(ymd, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) {
        if (n) out[0] = '\0';
        return;
    }
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_hour  = 12;   /* noon avoids DST edge cases */
    mktime(&t);
    int wd = t.tm_wday;   /* 0 = Sunday */
    if (lang_get() == LANG_ZH_CN) {
        static const char *zh[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(out, n, "%s %02d-%02d", zh[wd], t.tm_mon + 1, t.tm_mday);
    } else {
        static const char *en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        snprintf(out, n, "%s %02d-%02d", en[wd], t.tm_mon + 1, t.tm_mday);
    }
}

static void on_city(lv_event_t *e) {
    (void)e;
    weather_nav_push(&g_weather_app, PAGE_CURRENT, PAGE_CITY);
}

static void on_refresh(lv_event_t *e) {
    (void)e;
    weather_controller_refresh(&g_weather_app);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, uint32_t bg, uint32_t fg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = make_label(b, text, g_cjk_font, fg);
    lv_obj_center(l);
    return b;
}

static void render_forecast_row(lv_obj_t *parent, const char *day, const char *wtext, const char *trange) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dl = make_label(row, day, g_cjk_font, 0x333333);
    lv_obj_set_width(dl, 88);
    make_label(row, wtext, g_cjk_font, 0x666666);
    make_label(row, trange, g_cjk_font, 0x333333);
}

static lv_obj_t *build_current_page(struct WeatherApp *app, void *user_data) {
    (void)user_data;
    WeatherModel *m = app->model;
    if (m) m->current_page = PAGE_CURRENT;

    const char *title = (m && m->city[0]) ? m->city : tr(STR_APP_WEATHER);
    Page page = lv_page_create(title, false, NULL, NULL);
    lv_obj_t *cont = page.container;
    lv_home_indicator_create(page.screen);

    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    if (!m || !m->has_data) {
        /* loading / error / idle → centered message + a City button. */
        const char *msg = (m && m->error[0]) ? m->error : tr(STR_WEATHER_LOADING);
        lv_obj_t *lbl = make_label(cont, msg, g_cjk_font, 0x666666);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t *btn = lv_button_create(cont);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(btn, 150, 40);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
        lv_obj_t *bl = make_label(btn, tr(STR_WEATHER_CITY), g_cjk_font, 0xFFFFFF);
        lv_obj_center(bl);
        lv_obj_add_event_cb(btn, on_city, LV_EVENT_CLICKED, NULL);
        return page.screen;
    }

    /* ── current weather card ── */
    lv_obj_t *card = lv_obj_create(cont);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xE3F2FD), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *trow = lv_obj_create(card);
    lv_obj_set_size(trow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(trow, 0, 0);
    lv_obj_set_style_border_width(trow, 0, 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%.0f", m->temp);
    make_label(trow, tbuf, &lv_font_montserrat_20, 0x1565C0);
    make_label(trow, "°C", g_cjk_font, 0x1565C0);

    make_label(card, m->text, g_cjk_font, 0x333333);

    char info[160];
    snprintf(info, sizeof(info), "%s %.0f° · %s %.0f%% · %s %.0f km/h",
             tr(STR_WEATHER_FEELS_LIKE), m->feels_like,
             tr(STR_WEATHER_HUMIDITY), m->humidity,
             tr(STR_WEATHER_WIND), m->wind);
    make_label(card, info, g_cjk_font, 0x555555);

    if (m->updated[0]) {
        char upd[64];
        snprintf(upd, sizeof(upd), "%s %s", tr(STR_WEATHER_UPDATED), m->updated);
        make_label(card, upd, g_cjk_font, 0x999999);
    }

    /* ── 7-day forecast ── */
    make_label(cont, tr(STR_WEATHER_FORECAST), g_cjk_font, 0x333333);

    for (int i = 0; i < WEATHER_DAYS; i++) {
        char day[32], range[32];
        format_day(day, sizeof(day), m->daily_date[i]);
        snprintf(range, sizeof(range), "%.0f° / %.0f°", m->tmax[i], m->tmin[i]);
        render_forecast_row(cont, day, m->daily_text[i], range);
    }

    /* ── bottom action buttons ── */
    lv_obj_t *btnrow = lv_obj_create(cont);
    lv_obj_set_size(btnrow, LV_PCT(100), 44);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_column(btnrow, 8, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);

    make_btn(btnrow, tr(STR_WEATHER_CITY), on_city, 0xEEEEEE, 0x333333);
    make_btn(btnrow, tr(STR_WEATHER_REFRESH), on_refresh, 0x1E88E5, 0xFFFFFF);

    return page.screen;
}

void weather_view_current_init_registry(struct WeatherApp *app) {
    PAGE_REGISTE(app, PAGE_CURRENT, build_current_page);
}
