/*
 * NomadCast — FlashDB validation & performance test (debug/t_flashdb)
 *
 * Standalone project to validate FlashDB KVDB (file mode) on the Leisound V1 SD
 * card, and measure its performance vs the current JSON full-rewrite approach.
 *
 * FlashDB source: vendored at repo-root vendor/FlashDB, wired as a local
 *                 component in debug/t_flashdb/components/flashdb/ (file mode).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "flashdb.h"

static const char *TAG = "t_flashdb";

/* ── Leisound V1 pins ─────────────────────────────────────────────────────── */
#define PIN_EN_POWER  GPIO_NUM_46
#define PIN_SD_CLK    GPIO_NUM_1
#define PIN_SD_CMD    GPIO_NUM_14
#define PIN_SD_DAT0   GPIO_NUM_2
#define MOUNT_POINT   "/sdcard"
#define DB_DIR        "/sdcard/.podcast/db"

/* KVDB sizing. File mode stores one file per sector, and init formats EVERY
 * sector once (a header write per sector). So use a LARGE sector to keep the
 * sector/file count low: 256KB × 80 = 20MB — holds the N=50000 stress case
 * (~12.5MB live, insert-only) while only creating/formatting ~80 files. */
#define DB_SEC_SIZE   (256 * 1024)
#define DB_MAX_SIZE   (20 * 1024 * 1024)

static struct fdb_kvdb s_kvdb;

/* ── SD mount (from debug/t_sd) ───────────────────────────────────────────── */

static sdmmc_card_t *sd_mount(void)
{
    gpio_set_direction(PIN_EN_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_EN_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    fflush(stdout); vTaskDelay(pdMS_TO_TICKS(100));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK; slot.cmd = PIN_SD_CMD; slot.d0 = PIN_SD_DAT0; slot.width = 1;
    esp_vfs_fat_mount_config_t mcfg = { .format_if_mount_failed = false, .max_files = 8,
                                        .allocation_unit_size = 16 * 1024 };
    sdmmc_card_t *card = NULL;
    esp_err_t r = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (r != ESP_OK) { ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(r)); return NULL; }
    ESP_LOGI(TAG, "SD mounted");
    return card;
}

/* ── FlashDB KVDB (file mode on SD) ───────────────────────────────────────── */

static void ensure_db_dir(void)
{
    struct stat st;
    if (stat("/sdcard/.podcast", &st) != 0) mkdir("/sdcard/.podcast", 0755);
    if (stat(DB_DIR, &st) != 0) mkdir(DB_DIR, 0755);
}

static bool db_open(void)
{
    ensure_db_dir();
    uint32_t sec_size = DB_SEC_SIZE;
    uint32_t max_size = DB_MAX_SIZE;
    bool file_mode = true;
    memset(&s_kvdb, 0, sizeof(s_kvdb));
    fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_size);
    fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_MAX_SIZE, &max_size);
    fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_FILE_MODE, &file_mode);
    fdb_err_t err = fdb_kvdb_init(&s_kvdb, "podcast", DB_DIR, NULL, NULL);
    if (err != FDB_NO_ERR) { ESP_LOGE(TAG, "fdb_kvdb_init failed: %d", err); return false; }
    ESP_LOGI(TAG, "KVDB opened at %s (sec=%uKB, max=%uMB)",
             DB_DIR, (unsigned)(DB_SEC_SIZE / 1024), (unsigned)(DB_MAX_SIZE / (1024 * 1024)));
    return true;
}

/* ── KV helpers (key = hash(url), value = ~150B JSON) ─────────────────────── */

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
static void keyhex(char out[9], const char *url) { snprintf(out, 9, "%08x", (unsigned)fnv1a(url)); }

/* ~150-byte representative episode JSON for synthetic episode index i */
static void mkval(char *out, int outsz, int i) {
    snprintf(out, outsz,
      "{\"cid\":%d,\"col\":1559695855,\"ch\":\"KnowledgeBar\",\"ep\":\"Episode number %d title padding text\",\"url\":\"url_%d\",\"dur\":%d}",
      i % 50, i, i, 60 + (i % 40));
}

static bool kv_put(const char *key, const char *val) {
    struct fdb_blob b;
    return fdb_kv_set_blob(&s_kvdb, key, fdb_blob_make(&b, val, strlen(val) + 1)) == FDB_NO_ERR;
}
static bool kv_get(const char *key, char *buf, int bufsz) {
    struct fdb_blob b;
    size_t n = fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&b, buf, bufsz));
    if (n == 0) return false;
    buf[(n < (size_t)bufsz) ? n - 1 : bufsz - 1] = '\0';  /* value was stored with its NUL */
    return true;
}
static bool kv_del(const char *key) { return fdb_kv_del(&s_kvdb, key) == FDB_NO_ERR; }

static int kv_count(void) {
    struct fdb_kv_iterator it; fdb_kv_iterator_init(&s_kvdb, &it);
    int n = 0; while (fdb_kv_iterate(&s_kvdb, &it)) n++;
    return n;
}

/* ── Functional test: CRUD + dedup ────────────────────────────────────────── */

static bool test_functional(void) {
    ESP_LOGI(TAG, "--- Functional ---");
    char key[9]; keyhex(key, "url_smoke");
    char val[256], got[256];

    mkval(val, sizeof(val), 1);
    if (!kv_put(key, val)) { ESP_LOGE(TAG, "put fail"); return false; }
    if (!kv_get(key, got, sizeof(got)) || strcmp(got, val)) { ESP_LOGE(TAG, "get mismatch"); return false; }
    ESP_LOGI(TAG, "  CRUD put/get OK");

    /* dedup: same key, new value → still ONE entry, latest value */
    int before = kv_count();
    mkval(val, sizeof(val), 2);
    kv_put(key, val);
    if (kv_count() != before) { ESP_LOGE(TAG, "dedup: count changed (%d->%d)", before, kv_count()); return false; }
    kv_get(key, got, sizeof(got));
    if (strcmp(got, val)) { ESP_LOGE(TAG, "dedup: not latest"); return false; }
    ESP_LOGI(TAG, "  dedup/overwrite OK");

    if (!kv_del(key) || kv_get(key, got, sizeof(got))) { ESP_LOGE(TAG, "del fail"); return false; }
    ESP_LOGI(TAG, "  delete OK");
    ESP_LOGI(TAG, "--- Functional PASS ---");
    return true;
}

/* ── Persistence across reboot ────────────────────────────────────────────── */

static void test_persist(void) {
    ESP_LOGI(TAG, "--- Persistence ---");
    char key[9]; keyhex(key, "url_sentinel");
    char got[64];
    if (kv_get(key, got, sizeof(got))) {
        ESP_LOGI(TAG, "  survived reboot: '%s'  --- Persistence PASS ---", got);
    } else {
        kv_put(key, "sentinel_v1");
        ESP_LOGW(TAG, "  wrote sentinel; RESET/power-cycle the board (do NOT reflash) to confirm it survives");
    }
}

/* ── Performance ──────────────────────────────────────────────────────────
 * fdb_kv_set_default() formats all sectors and (no default KVs) leaves the DB
 * EMPTY — a clean wipe, so each N is measured from an empty DB. NOTE: this also
 * wipes the persistence sentinel, so on reboots after a perf run the persistence
 * test re-prints "wrote sentinel" (persistence was already confirmed in Task 4). */

static void bench_flashdb(int N) {
    char key[9], val[256], got[256];
    fdb_kv_set_default(&s_kvdb);                 /* wipe → empty */

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        char u[24]; snprintf(u, sizeof(u), "perf_%d", i);
        keyhex(key, u); mkval(val, sizeof(val), i); kv_put(key, val);
    }
    int64_t t_ins = esp_timer_get_time() - t0;

    /* full iterate (= boot-load cost) */
    t0 = esp_timer_get_time();
    struct fdb_kv_iterator it; fdb_kv_iterator_init(&s_kvdb, &it);
    struct fdb_blob b; int seen = 0;
    while (fdb_kv_iterate(&s_kvdb, &it)) {
        fdb_blob_read((fdb_db_t)&s_kvdb, fdb_kv_to_blob(&it.curr_kv, fdb_blob_make(&b, got, sizeof(got))));
        seen++;
    }
    int64_t t_iter = esp_timer_get_time() - t0;

    /* single get + single overwrite */
    { char u[24]; snprintf(u, sizeof(u), "perf_%d", N / 2); keyhex(key, u); }
    t0 = esp_timer_get_time(); kv_get(key, got, sizeof(got)); int64_t t_get = esp_timer_get_time() - t0;
    mkval(val, sizeof(val), 999999);
    t0 = esp_timer_get_time(); kv_put(key, val); int64_t t_ovr = esp_timer_get_time() - t0;

    ESP_LOGI(TAG, "FDB  N=%-6d insert=%lldms (%lldus/op)  iterate=%lldms (seen=%d)  get=%lldus  overwrite=%lldus",
             N, (long long)(t_ins / 1000), (long long)(t_ins / N),
             (long long)(t_iter / 1000), seen, (long long)t_get, (long long)t_ovr);
}

/* Baseline: emulate today's system — after each of the first N adds, rewrite the
 * whole JSON file containing k entries (cumulative → O(N^2)). N<=500 only. */
static void bench_json_baseline(int N) {
    const char *path = "/sdcard/.podcast/baseline.json";
    int64_t t0 = esp_timer_get_time();
    for (int k = 1; k <= N; k++) {
        FILE *f = fopen(path, "wb");
        if (!f) { ESP_LOGE(TAG, "baseline open fail"); return; }
        fprintf(f, "{\"episodes\":[\n");
        char val[256];
        for (int i = 0; i < k; i++) { mkval(val, sizeof(val), i); fprintf(f, "%s%s\n", i ? "," : "", val); }
        fprintf(f, "]}\n");
        fclose(f);
    }
    int64_t t = esp_timer_get_time() - t0;
    unlink(path);
    ESP_LOGI(TAG, "JSON N=%-6d cumulative-rewrite=%lldms (O(N^2))", N, (long long)(t / 1000));
}

static void perf_run(void) {
    ESP_LOGI(TAG, "--- Performance ---");
    int Ns[] = {50, 200, 500, 5000, 50000};
    for (int j = 0; j < 5; j++) {
        int N = Ns[j];
        bench_flashdb(N);
        if (N <= 500) bench_json_baseline(N);
        else ESP_LOGI(TAG, "JSON N=%-6d skipped (extrapolated: O(N^2), far worse)", N);
    }
    ESP_LOGI(TAG, "--- Performance DONE ---");
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "========== t_flashdb ==========");
    if (!sd_mount()) { goto fail; }
    if (!db_open())  { goto fail; }
    ESP_LOGI(TAG, "--- INIT PASS ---");
    if (!test_functional()) { goto fail; }
    test_persist();
    perf_run();
    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); ESP_LOGI(TAG, "STATUS: TESTS DONE"); }
fail:
    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); ESP_LOGE(TAG, "STATUS: TEST FAIL"); }
}
