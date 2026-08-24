# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NomadCast — ESP32-S3 smart podcast player firmware for the **Leisound V1** board.

- **MCU**: ESP32-S3 (16MB flash, 8MB PSRAM)
- **Display**: ST7789V 240×320 RGB565 (SPI2, esp_lcd)
- **Touch**: GT911 (software bit-bang I2C — SDA=38 / SCL=45)
- **Audio**: ES8156 DAC + HT6872 amp (I2S)
- **SD**: 1-bit SDMMC (GPIO 10/11/9)
- **Framework**: ESP-IDF v5.5.3
- **UI**: LVGL v9.5 (managed component)
- **PC simulator**: `pc_demo/` (SDL2 + LVGL, shares `apps/` source)

## Build

```bash
# ESP-IDF must be at C:/Espressif/frameworks/esp-idf-v5.5.3/
idf.py set-target esp32s3    # first time only
idf.py build flash monitor   # COM port auto-detect
```

Key sdkconfig settings (in `sdkconfig.defaults`):
- `CONFIG_IDF_TARGET="esp32s3"`
- `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`
- `CONFIG_FATFS_LFN_HEAP=y`
- `CONFIG_LV_USE_BUILTIN_MALLOC=y`
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
- `CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY=y`

**GCC ICE workaround**: if `esp_lcd_panel_rgb.c` crashes the compiler, set `IDF_CCACHE_ENABLE=0` and rebuild clean.

## Directory Structure

```
NomadCast/
├── main/                     # App entry point
│   ├── NomadCast.cpp        # app_main: HW init → LVGL → apps → event loop
│   ├── CMakeLists.txt
│   └── idf_component.yml     # lvgl/lvgl: ^9
│
├── apps/                     # MVC apps (shared with pc_demo)
│   ├── settings/             # Settings app (full — 6 subpages)
│   │   ├── app.c/.h          # Self-registers via settings_app_register()
│   │   ├── model.c/.h        # WiFi, storage, update state
│   │   ├── view.c/.h          # page_navigator + subpage registry
│   │   ├── controller.c/.h
│   │   └── subpages/         # main, general, wifi, wifi_connect, storage, update
│   └── podcast/              # Podcast app (stub — full UI excluded due to DRAM)
│       ├── app.c/.h          # Placeholder "Coming Soon" page
│       ├── model.c/.h        # Full data model (channels, episodes, downloads)
│       ├── view.c/.h          # Full view layer (not compiled)
│       └── subpages/         # Full subpages (not compiled)
│
├── sys/                      # System modules (ESP-IDF components)
│   ├── uilv/                 # Shared LVGL layer
│   │   ├── framework/        # page_navigator (push/pop navigation stack)
│   │   ├── widgets/          # lv_page, lv_status_bar, lv_toast, lv_bottom_sheet, lv_num_input
│   │   ├── imgs/24x24/       # Icon C arrays (ic_settings, ic_podcasts, ic_wifi, etc.)
│   │   ├── theme/            # ls_theme
│   │   ├── fonts/            # CJK fonts
│   │   └── utils/            # ui_utils
│   ├── app_manager/          # App lifecycle: register → start/stop/back dispatch
│   ├── launcher/             # Home screen: 2-col grid of app icons (80px)
│   │   ├── launcher.c/.h     # open_app, on_app_close, return_home
│   │   ├── launcher_home_ui.c # Grid UI
│   │   └── launcher_gesture.c # Swipe-up (exit confirm) + swipe-down (control panel)
│   ├── input/                # Physical keys via espressif/button
│   ├── clock/                # NTP time sync
│   ├── flash_store/          # NVS key-value storage
│   └── sleep_monitor/        # Sleep/wake management
│
├── components/               # Legacy C++ BSP (EXCLUDED from build)
│   ├── audio/ board_leisound/ battery/ codec_board/
│   ├── display/ i2c_bsp/ touch_bsp/ wifi/
│   └── button/ flash/ rtc/   # (empty — no CMakeLists)
│
├── drivers/                  # Device drivers (active)
│   ├── es8156/               # ES8156 DAC (software I2C)
│   ├── gt911/                # GT911 touch controller (software I2C)
│   └── sw_i2c/               # Shared software bit-bang I2C (SDA=38 / SCL=45)
│
├── board/
│   ├── nomadcast_v1.h        # Pin definitions (single source of truth, NOMADCAST_*)
│   └── leisound_v1.h         # Deprecated shim → nomadcast_v1.h (LEISOUND_* aliases)
│
├── pc_demo/                  # PC simulator (SDL2 + LVGL)
│   ├── CMakeLists.txt        # References ../../apps/settings/*.c, ../../apps/podcast/*.c
│   ├── main.c, hal.c, lv_conf.h
│   └── podcast/backend.c, cache.c   # PC-only (WinHTTP)
│
├── debug/                    # Feature validation projects
│   ├── t_display_touch/      # Display + touch driver validation
│   ├── t_ui/                 # LVGL UI test (button + swipe)
│   ├── t_wifi/               # WiFi scan + HTTP download test
│   ├── t_play/               # M4A audio player (ADF pipeline)
│   ├── t_key/                # Physical key test
│   └── t_speaker/ t_hello/ t_sd/ t_touch/ t_earphone/ t_earphone2/ t_display/
│
├── sdkconfig.defaults        # Kconfig defaults (all critical settings)
└── dependencies.lock         # Managed component versions
```

## Architecture

### Hardware Init (proven from t_ui)

All display/touch init is inline in `main/NomadCast.cpp`:
1. Power ON (AP power GPIO46 + LCD power GPIO43 + backlight GPIO12) → HW reset (shared GPIO40 for LCD + GT911)
2. SPI2 + ST7789 via `esp_lcd` framework (`reset_gpio_num = GPIO_NUM_NC`)
3. GT911 via `drivers/gt911` (software bit-bang I2C via `drivers/sw_i2c`)
4. LVGL display + indev + `esp_timer` tick (1ms)
5. LVGL buffers: 2 × (240×20) from PSRAM, partial render mode
6. RGB565 byte swap in flush callback (LVGL big-endian → ST7789 little-endian)

### App Lifecycle (EPOS pattern)

Each app in `apps/<name>/app.c` defines its own `application_t` struct and registers via `app_manager_add_application()`:

```c
// apps/settings/app.c
static application_t settings_app_desc = {
    .name = "Settings", .icon = &ic_settings,
    .start_func = settings_app_start, .stop_func = settings_app_stop,
    .back_func = settings_app_back, .category = APP_CATEGORY_SYSTEM,
};
void settings_app_register(void) { app_manager_add_application(&settings_app_desc); }
```

`main/NomadCast.cpp` calls register functions, then `launcher_home_ui()` renders the icon grid:

```cpp
app_manager_init();
settings_app_register();
podcast_app_register();
launcher_home_ui();
```

`app_manager_show(name)` → LVGL timer → `start_func(root, group)` → app builds its UI.
`app_manager_exit_app()` → `stop_func()` → tear down. State machine: STOPPED ↔ UI_VISIBLE ↔ UI_HIDDEN.

### LVGL Integration

- Display: `lv_display_create(240, 320)` + flush callback via `esp_lcd_panel_draw_bitmap`
- Touch: `lv_indev_create(LV_INDEV_TYPE_POINTER)` + GT911 I2C polling
- Tick: `esp_timer` 1ms periodic → `lv_tick_inc(1)`
- Event loop: `lv_timer_handler()` with minimum 5ms delay

### Shared Source with PC Simulator

`apps/settings/` and `apps/podcast/` are the canonical source. `pc_demo/CMakeLists.txt` references them via relative paths (`../../apps/settings/*.c`). PC-specific files (WinHTTP backend, local filesystem cache) stay in `pc_demo/`. ESP-IDF stubs (`backend_esp.c`, `cache_esp.c`, `hal.h`) are in `apps/podcast/`.

### DRAM Constraint

Internal SRAM is ~333KB (dram0_0_seg). With PSRAM enabled, heap allocations use `MALLOC_CAP_SPIRAM`. BSS/data must fit in internal DRAM. The full podcast component (all subpages, controller, model) currently exceeds this — only `app.c` is compiled. Solutions: wrap large Win32 code in `#ifdef _WIN32`, use `EXT_RAM_BSS_ATTR`, or enable `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.

### Download Tasks: memory ↔ JSON ↔ audio file

A download task exists in three related places. Understand the split before touching `controller.c` / `task_store.c`:

| Layer | Where | Role |
|---|---|---|
| **In-RAM** | `model->download_tasks[]` (`DownloadTask`) | Runtime source of truth — UI + worker read/write it |
| **Persistent** | `/sdcard/.podcast/cache/download_tasks/%08d.json` (one file per task, id = filename) | Survives reboot; field-for-field serialization of `DownloadTask` (via `task_to_json`/`task_from_json`) — **metadata only, not audio** |
| **Audio file** | `/sdcard/.podcast/downloads/<channel>/<episode>.m4a` (`DL_BASE_PATH`) | The actual downloaded audio; managed separately by `local_cache` |

**Sync rules (keep both copies consistent):**
- Create: `task_store_create` = grow `download_tasks[]` (realloc) **and** `write_task_file` (JSON), together.
- Any state change (PENDING→DOWNLOADING→COMPLETED/FAILED, progress): call `task_store_update(app, id)` to rewrite that one JSON — this is why a power-cut status is durable, and how `dl_resume_pending` detects a mid-download task on boot and resets it to PENDING.
- Delete/cancel: `task_store_delete` = `unlink` JSON **and** remove from `download_tasks[]`.
- Boot: `task_store_load` scans the dir → rebuilds `download_tasks[]` (only entries within `TASK_TTL_DAYS` = 3 days); `task_store_purge_old` deletes older JSON.

**Download worker (`controller.c`)** drives directly off `download_tasks[]` — there is **no separate in-RAM job queue**. The worker (CPU1) scans for the smallest-id PENDING task, copies `audio_url`/`file_path` to static scratch, downloads (lock-free), sets status, and hands the finished `task_id` to the LVGL thread via a small SPSC ring (`g_dl_done_ids`) so `controller_process_download` fires `APP_EVENT_DOWNLOAD_COMPLETED` + `cache_local_add`. Cancellation: `g_dl_abort_current` + `g_dl_current_task_id` make `dl_progress_cb` return false → `http_download_to_file` aborts and unlinks the partial file. Downloads are serial (one at a time).

**TTL asymmetry:** the task *list* is 3-day history (JSON purged after `TASK_TTL_DAYS`); the downloaded *audio* under `downloads/` persists independently in the local library.

## Key Patterns

- **No ADF**: The `vendor/esp-adf/` and legacy C++ BSP components are excluded. All init is inline C/C++.
- **Shared fonts/icons via `sys/uilv`**: All components `REQUIRES uilv` to avoid duplication.
- **`printf` for cross-platform logging**: Works on both PC and ESP-IDF (USB Serial/JTAG).
- **Physical keys use `espressif/button` v4.x**: `iot_button_new_gpio_device()` + `BUTTON_SINGLE_CLICK` / `BUTTON_LONG_PRESS_START`.
- **sdkconfig management**: Edit `sdkconfig.defaults` for permanent settings; `set-target` regenerates from defaults. Manually edit `sdkconfig` only when needed — it's wiped by `set-target`.
