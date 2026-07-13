/**
 * @file model.c — Podcast data model (Channel/Episode)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "model.h"
#include "app.h"
#include "flash_store.h"
#include "esp_mac.h"

static void s_strcpy(char *dst, const char *src, size_t sz) {
    if (sz > 0) { strncpy(dst, src ? src : "", sz - 1); dst[sz - 1] = '\0'; }
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void podcast_model_init(struct PodcastApp *app)
{
    app->model = (PodcastModel *)calloc(1, sizeof(PodcastModel));
    if (app->model) {
        app->model->net_state = NET_STATE_IDLE;
        app->model->font_size = flash_get_i32("podcast", "font", 2);          /* default Large */
        app->model->download_quality = flash_get_i32("podcast", "qual", 1);   /* default Medium */

        /* Restore saved login credentials from NVS */
        char saved_user[64], saved_pass[64], saved_id[64];
        if (flash_get_str("podcast", "user", saved_user, sizeof(saved_user), "") > 0 &&
            saved_user[0]) {
            flash_get_str("podcast", "pass", saved_pass, sizeof(saved_pass), "");
            flash_get_str("podcast", "uid",  saved_id,  sizeof(saved_id), "");
            s_strcpy(app->model->username, saved_user, sizeof(app->model->username));
            s_strcpy(app->model->password, saved_pass, sizeof(app->model->password));
            s_strcpy(app->model->user_id,  saved_id,  sizeof(app->model->user_id));
            app->model->logged_in = true;
            printf("[MODEL] Auto-login: '%s' id=%s\n", saved_user, saved_id); fflush(stdout);
        }

        /* Device ID — generated once, persisted forever */
        if (flash_get_str("podcast", "devid",
                          app->model->device_id, sizeof(app->model->device_id), "") == 0 ||
            !app->model->device_id[0]) {
            /* First boot — generate from MAC address */
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(app->model->device_id, sizeof(app->model->device_id),
                     "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            flash_set_str("podcast", "devid", app->model->device_id);
            printf("[MODEL] Device ID generated: %s\n", app->model->device_id);
            fflush(stdout);
        }
    }
}

void podcast_model_deinit(struct PodcastApp *app)
{
    if (!app->model) return;
    PodcastModel *m = app->model;
    free(m->network_channels); free(m->network_episodes);
    free(m->local_channels); free(m->local_episodes);
    free(m->queue); free(m->download_tasks);
    free(m->search_results.channels); free(m->search_results.episodes);
    free(m->current_channel_episodes);
    free(m); app->model = NULL;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static Channel *find_channel_by_id(PodcastModel *m, int channel_id) {
    for (int i = 0; i < m->network_channel_count; i++)
        if (m->network_channels[i].id == channel_id) return &m->network_channels[i];
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].id == channel_id) return &m->local_channels[i];
    return NULL;
}

static Episode *find_episode_by_id(PodcastModel *m, int episode_id) {
    for (int i = 0; i < m->network_episode_count; i++)
        if (m->network_episodes[i].id == episode_id) return &m->network_episodes[i];
    for (int i = 0; i < m->local_episode_count; i++)
        if (m->local_episodes[i].id == episode_id) return &m->local_episodes[i];
    return NULL;
}

/* ── Data import ──────────────────────────────────────────────────────── */

void podcast_model_import_channels(struct PodcastApp *app, Channel *channels, int count)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->network_channels);
    m->network_channels = channels; m->network_channel_count = count;
    for (int c = 0; c < CHANNEL_CATEGORY_COUNT; c++)
        m->network_channels_shown[c] = 0;  /* offset 0 = first page */
    printf("[MODEL] Imported %d channels\n", count); fflush(stdout);
}

void podcast_model_append_channels(struct PodcastApp *app, Channel *channels, int count)
{
    PodcastModel *m = app->model; if (!m || count <= 0) { free(channels); return; }
    int nc = m->network_channel_count + count;
    Channel *na = (Channel *)realloc(m->network_channels, nc * sizeof(Channel));
    if (!na) { free(channels); return; }
    memcpy(na + m->network_channel_count, channels, count * sizeof(Channel));
    free(channels); m->network_channels = na; m->network_channel_count = nc;
}

void podcast_model_import_episodes(struct PodcastApp *app, Episode *episodes, int count)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->network_episodes);
    m->network_episodes = episodes; m->network_episode_count = count;
}

void podcast_model_set_search_results(struct PodcastApp *app,
    Channel *channels, int cc, Episode *episodes, int ec, const char *query)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->search_results.channels); free(m->search_results.episodes);
    m->search_results.channels = channels; m->search_results.channel_count = cc;
    m->search_results.episodes = episodes; m->search_results.episode_count = ec;
    if (query) s_strcpy(m->search_results.query, query, sizeof(m->search_results.query));
}

void podcast_model_clear_search_results(struct PodcastApp *app)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->search_results.channels); m->search_results.channels = NULL;
    free(m->search_results.episodes); m->search_results.episodes = NULL;
    m->search_results.channel_count = m->search_results.episode_count = 0;
}

void podcast_model_set_net_state(struct PodcastApp *app, net_state_t state, const char *error)
{
    PodcastModel *m = app->model; if (!m) return;
    m->net_state = state;
    if (error) { s_strcpy(m->net_error, error, sizeof(m->net_error)); }
    else m->net_error[0] = '\0';
}

/* ── Channel queries ──────────────────────────────────────────────────── */

const Channel *podcast_model_get_channel_by_id(struct PodcastApp *app, int channel_id)
{
    if (!app->model) return NULL;
    for (int i = 0; i < app->model->search_results.channel_count; i++)
        if (app->model->search_results.channels[i].id == channel_id)
            return &app->model->search_results.channels[i];
    return find_channel_by_id(app->model, channel_id);
}

int podcast_model_get_channel_count_by_category(struct PodcastApp *app, channel_category_t cat)
{
    PodcastModel *m = app->model; if (!m) return 0;
    int c = 0;
    for (int i = 0; i < m->network_channel_count; i++)
        if (m->network_channels[i].category == cat) c++;
    return c;
}

const Channel **podcast_model_get_channels_by_category(struct PodcastApp *app, channel_category_t cat, int *out)
{
    PodcastModel *m = app->model; *out = 0; if (!m) return NULL;
    int n = podcast_model_get_channel_count_by_category(app, cat);
    if (!n) return NULL;
    const Channel **r = (const Channel **)malloc(sizeof(Channel *) * n);
    if (!r) return NULL;
    int idx = 0;
    for (int i = 0; i < m->network_channel_count; i++)
        if (m->network_channels[i].category == cat) r[idx++] = &m->network_channels[i];
    *out = n; return r;
}

/* ── Episode queries ──────────────────────────────────────────────────── */

const Episode **podcast_model_get_episodes_by_channel(struct PodcastApp *app, int channel_id, int *out)
{
    PodcastModel *m = app->model; *out = 0; if (!m) return NULL;
    if (m->current_channel && m->current_channel->id == channel_id && m->current_channel_episodes) {
        int n = m->current_channel_episode_count;
        const Episode **r = (const Episode **)malloc(sizeof(Episode *) * n);
        if (!r) return NULL;
        for (int i = 0; i < n; i++) r[i] = &m->current_channel_episodes[i];
        *out = n; return r;
    }
    int n = 0;
    for (int i = 0; i < m->network_episode_count; i++)
        if (m->network_episodes[i].channel_id == channel_id) n++;
    if (!n) return NULL;
    const Episode **r = (const Episode **)malloc(sizeof(Episode *) * n);
    if (!r) return NULL;
    int idx = 0;
    for (int i = 0; i < m->network_episode_count; i++)
        if (m->network_episodes[i].channel_id == channel_id) r[idx++] = &m->network_episodes[i];
    *out = n; return r;
}

const Episode *podcast_model_get_episode_by_id(struct PodcastApp *app, int episode_id)
{
    if (!app->model) return NULL;
    for (int i = 0; i < app->model->current_channel_episode_count; i++)
        if (app->model->current_channel_episodes[i].id == episode_id)
            return &app->model->current_channel_episodes[i];
    for (int i = 0; i < app->model->search_results.episode_count; i++)
        if (app->model->search_results.episodes[i].id == episode_id)
            return &app->model->search_results.episodes[i];
    return find_episode_by_id(app->model, episode_id);
}

/* ── Local content ────────────────────────────────────────────────────── */

const Channel **podcast_model_get_downloaded_channels_by_category(struct PodcastApp *app, channel_category_t cat, int *out)
{
    PodcastModel *m = app->model; *out = 0; if (!m) return NULL;
    int n = 0;
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].category == cat && m->local_channels[i].downloaded) n++;
    if (!n) return NULL;
    const Channel **r = (const Channel **)malloc(sizeof(Channel *) * n);
    if (!r) return NULL;
    int idx = 0;
    for (int i = 0; i < m->local_channel_count; i++)
        if (m->local_channels[i].category == cat && m->local_channels[i].downloaded) r[idx++] = &m->local_channels[i];
    *out = n; return r;
}

void podcast_model_set_local_state(struct PodcastApp *app, bool sd, bool has)
{ if (app->model) { app->model->local_sd_mounted = sd; app->model->local_has_content = has; } }
bool podcast_model_is_local_sd_mounted(struct PodcastApp *app) { return app->model && app->model->local_sd_mounted; }
bool podcast_model_has_local_content(struct PodcastApp *app) { return app->model && app->model->local_has_content; }

/* ── Player queue ─────────────────────────────────────────────────────── */

void podcast_model_set_queue(struct PodcastApp *app, const int *ids, int count)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->queue);
    m->queue = (int *)malloc(sizeof(int) * count);
    if (m->queue) {
        memcpy(m->queue, ids, sizeof(int) * count);
        m->queue_count = count; m->queue_index = 0;
        m->player_playing = false; m->current_episode_id = count ? ids[0] : 0;
    }
}
int podcast_model_queue_next(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    if (!m || !m->queue || m->queue_index >= m->queue_count - 1) return -1;
    m->queue_index++; m->current_episode_id = m->queue[m->queue_index]; return m->current_episode_id;
}
int podcast_model_queue_prev(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    if (!m || !m->queue || m->queue_index <= 0) return -1;
    m->queue_index--; m->current_episode_id = m->queue[m->queue_index]; return m->current_episode_id;
}
int podcast_model_queue_current(struct PodcastApp *app) {
    PodcastModel *m = app->model;
    return (m && m->queue && m->queue_count > 0) ? m->queue[m->queue_index] : -1;
}
void podcast_model_set_playing(struct PodcastApp *app, bool p) { if (app->model) app->model->player_playing = p; }
bool podcast_model_is_playing(struct PodcastApp *app) { return app->model && app->model->player_playing; }
int podcast_model_get_current_episode_id(struct PodcastApp *app) { return app->model ? app->model->current_episode_id : 0; }

/* ── Search history ───────────────────────────────────────────────────── */

void podcast_model_add_search_history(struct PodcastApp *app, const char *q)
{
    PodcastModel *m = app->model; if (!m || !q || !q[0]) return;
    for (int i = 0; i < m->search_history_count; i++) {
        if (strcmp(m->search_history[i], q) == 0) {
            char t[64]; s_strcpy(t, m->search_history[i], sizeof(t));
            for (int j = i; j > 0; j--) s_strcpy(m->search_history[j], m->search_history[j-1], sizeof(m->search_history[j]));
            s_strcpy(m->search_history[0], t, sizeof(m->search_history[0])); return;
        }
    }
    int ins = m->search_history_count >= 10 ? 9 : m->search_history_count;
    for (int i = ins; i > 0; i--) s_strcpy(m->search_history[i], m->search_history[i-1], sizeof(m->search_history[i]));
    s_strcpy(m->search_history[0], q, sizeof(m->search_history[0]));
    if (m->search_history_count < 10) m->search_history_count++;
}
int podcast_model_get_search_history_count(struct PodcastApp *app) { return app->model ? app->model->search_history_count : 0; }
const char *podcast_model_get_search_history_item(struct PodcastApp *app, int idx) {
    return (app->model && idx >= 0 && idx < app->model->search_history_count) ? app->model->search_history[idx] : NULL;
}

/* ── Download tasks ───────────────────────────────────────────────────── */

const DownloadTask *podcast_model_get_download_tasks(struct PodcastApp *app, int *out) {
    *out = app->model ? app->model->download_task_count : 0;
    return app->model ? app->model->download_tasks : NULL;
}
int podcast_model_get_pending_download_count(struct PodcastApp *app) {
    if (!app->model) return 0;
    int c = 0;
    for (int i = 0; i < app->model->download_task_count; i++)
        if (app->model->download_tasks[i].status <= DOWNLOAD_STATUS_DOWNLOADING) c++;
    return c;
}
static bool ia_has(const int *a, int n, int v) { for (int i=0;i<n;i++) if(a[i]==v) return true; return false; }
void podcast_model_delete_download_tasks(struct PodcastApp *app, const int *ids, int n) {
    PodcastModel *m = app->model; if (!m || n <= 0) return;
    int w = 0;
    for (int i = 0; i < m->download_task_count; i++)
        if (!ia_has(ids, n, m->download_tasks[i].id)) { if (w != i) m->download_tasks[w] = m->download_tasks[i]; w++; }
    m->download_task_count = w;
}
void podcast_model_cancel_download_tasks(struct PodcastApp *app, const int *ids, int n) {
    PodcastModel *m = app->model; if (!m || n <= 0) return;
    int w = 0;
    for (int i = 0; i < m->download_task_count; i++) {
        if (ia_has(ids, n, m->download_tasks[i].id)) {
            download_status_t s = m->download_tasks[i].status;
            if (s == DOWNLOAD_STATUS_PENDING || s == DOWNLOAD_STATUS_DOWNLOADING) continue;
        }
        if (w != i) m->download_tasks[w] = m->download_tasks[i];
        w++;
    }
    m->download_task_count = w;
}

/* ── Login ────────────────────────────────────────────────────────────── */

void podcast_model_set_login(struct PodcastApp *app, const char *u, const char *p, const char *uid)
{
    PodcastModel *m = app->model;
    if (!m) return;
    m->logged_in = true;
    s_strcpy(m->username, u, sizeof(m->username));
    s_strcpy(m->password, p, sizeof(m->password));
    if (uid) s_strcpy(m->user_id, uid, sizeof(m->user_id));
    flash_set_str("podcast", "user", u);
    flash_set_str("podcast", "pass", p);
    if (uid) flash_set_str("podcast", "uid", uid);
    printf("[MODEL] Login saved: '%s' id=%s\n", u, uid ? uid : "(null)"); fflush(stdout);
}

void podcast_model_logout(struct PodcastApp *app)
{
    PodcastModel *m = app->model;
    if (!m) return;
    m->logged_in = false;
    m->username[0] = '\0';
    m->password[0] = '\0';
    m->user_id[0] = '\0';
    flash_set_str("podcast", "user", "");
    flash_set_str("podcast", "pass", "");
    flash_set_str("podcast", "uid", "");
    printf("[MODEL] Logout — credentials cleared\n"); fflush(stdout);
}

bool podcast_model_is_logged_in(struct PodcastApp *app) { return app->model && app->model->logged_in; }
const char *podcast_model_get_username(struct PodcastApp *app) { return (app->model && app->model->logged_in) ? app->model->username : NULL; }
const char *podcast_model_get_password(struct PodcastApp *app) { return (app->model && app->model->logged_in) ? app->model->password : NULL; }
const char *podcast_model_get_user_id(struct PodcastApp *app)   { return (app->model && app->model->logged_in) ? app->model->user_id : NULL; }
const char *podcast_model_get_device_id(struct PodcastApp *app) { return app->model ? app->model->device_id : NULL; }

/* ── User settings ─────────────────────────────────────────────────────── */

int podcast_model_get_font_size(struct PodcastApp *app) {
    return app->model ? app->model->font_size : 2;
}
void podcast_model_set_font_size(struct PodcastApp *app, int size) {
    if (!app->model) return;
    app->model->font_size = size;
    flash_set_i32("podcast", "font", size);
}

int podcast_model_get_download_quality(struct PodcastApp *app) {
    return app->model ? app->model->download_quality : 1;
}
void podcast_model_set_download_quality(struct PodcastApp *app, int quality) {
    if (!app->model) return;
    app->model->download_quality = quality;
    flash_set_i32("podcast", "qual", quality);
}




/* ── Network content pagination ───────────────────────────────────────── */

int podcast_model_get_network_total(struct PodcastApp *app, channel_category_t cat) { return podcast_model_get_channel_count_by_category(app, cat); }
const Channel **podcast_model_get_network_page(struct PodcastApp *app, channel_category_t cat, int off, int lim, int *out) {
    int n; const Channel **all = podcast_model_get_channels_by_category(app, cat, &n);
    if (!all) { *out = 0; return NULL; }
    int avail = n - off; if (avail <= 0) { free((void*)all); *out = 0; return NULL; }
    int cnt = avail < lim ? avail : lim;
    const Channel **p = (const Channel **)malloc(sizeof(Channel*) * cnt);
    if (!p) { free((void*)all); *out = 0; return NULL; }
    for (int i = 0; i < cnt; i++) p[i] = all[off + i];
    free((void*)all); *out = cnt; return p;
}
bool podcast_model_load_more_network_channels(struct PodcastApp *app, channel_category_t cat) {
    PodcastModel *m = app->model; if (!m) return false;
    int t = podcast_model_get_channel_count_by_category(app, cat);
    if (m->network_channels_shown[cat] >= t) return false;
    m->network_channels_shown[cat] += NETWORK_PAGE_SIZE;
    if (m->network_channels_shown[cat] > t) m->network_channels_shown[cat] = t;
    return true;
}
int podcast_model_get_network_shown(struct PodcastApp *app, channel_category_t cat) { return app->model ? app->model->network_channels_shown[cat] : 0; }
int podcast_model_get_network_active_category(struct PodcastApp *app) { return app->model ? app->model->network_active_category : 0; }
void podcast_model_set_network_active_category(struct PodcastApp *app, int cat) { if (app->model) app->model->network_active_category = cat; }
net_state_t podcast_model_get_net_state(struct PodcastApp *app) { return app->model ? app->model->net_state : NET_STATE_IDLE; }
const char *podcast_model_get_net_error(struct PodcastApp *app) {
    if (!app->model || !app->model->net_error[0]) return NULL;
    return app->model->net_error;
}

/* ── Current channel detail ───────────────────────────────────────────── */

void podcast_model_set_current_channel(struct PodcastApp *app, const Channel *channel, Episode *episodes, int n)
{
    PodcastModel *m = app->model; if (!m) return;
    free(m->current_channel_episodes);
    m->current_channel_episodes = episodes; m->current_channel_episode_count = n;
    m->current_channel_total_episodes = n; /* default: same as loaded */
    if (channel) m->current_channel = find_channel_by_id(m, channel->id);
}
const Channel *podcast_model_get_current_channel(struct PodcastApp *app) { return app->model ? app->model->current_channel : NULL; }
const Episode *podcast_model_get_current_channel_episode(struct PodcastApp *app, int idx) {
    return (app->model && idx >= 0 && idx < app->model->current_channel_episode_count) ? &app->model->current_channel_episodes[idx] : NULL;
}
int podcast_model_get_current_channel_episode_count(struct PodcastApp *app) { return app->model ? app->model->current_channel_episode_count : 0; }
