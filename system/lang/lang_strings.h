/*
 * NomadCast — UI string keys (i18n)
 *
 * One STR_* enum. Each language has a parallel `const char *const[]` array in
 * strings_<lang>.c whose entries are index-aligned with this enum. Add a key
 * here, then add the matching string to every language array (keep them in
 * the same order) and it is usable via tr(STR_xxx).
 */

#pragma once

enum {
    /* App display names */
    STR_APP_PODCAST = 0,
    STR_APP_SETTINGS,

    /* Settings — main menu */
    STR_GENERAL,
    STR_WIFI,
    STR_STORAGE,
    STR_UPDATE,
    STR_ABOUT,

    /* Settings — General */
    STR_TIMEZONE,
    STR_LANGUAGE,
    STR_24H_FORMAT,
    STR_SLEEP_TIMEOUT,
    STR_AUTO_POWER_OFF,
    STR_FACTORY_RESET,
    STR_FACTORY_RESET_WARN,
    STR_ERASE_AND_RESTART,
    STR_CANCEL,
    STR_SLEEP_OPTIONS,
    STR_POWER_OPTIONS,

    /* Settings — WiFi */
    STR_WIFI_CONNECTED,
    STR_CONNECTION_FAILED,
    STR_SCAN_FOR_NETWORKS,
    STR_SCANNING,
    STR_AVAILABLE_NETWORKS,

    /* Settings — WiFi Connect */
    STR_CONNECT,
    STR_ENTER_PASSWORD,
    STR_PASSWORD,
    STR_SHOW_PASSWORD,

    /* Settings — Storage */
    STR_NO_SD_CARD,
    STR_STORAGE_USAGE,
    STR_CLEAN_STORAGE,

    /* Settings — Update */
    STR_AUTO_UPDATE,
    STR_CHECK_FOR_UPDATE,
    STR_OTA_UPDATE,
    STR_OTA_WARN,
    STR_CONFIRM,
    STR_ALREADY_UP_TO_DATE,
    STR_RELEASED,
    STR_OTA_NOW,
    STR_URL_NOT_CONFIGURED,
    STR_SERVER_UNREACHABLE,
    STR_INVALID_SERVER_RESPONSE,
    STR_NO_NETWORK_CONNECTION,

    /* Settings — About */
    STR_DEVICE_NAME,
    STR_DEVICE_ID,
    STR_FIRMWARE_VER,
    STR_SDK,
    STR_HARDWARE,
    STR_LVGL,

    /* Settings — OTA status */
    STR_OTA_STATUS,
    STR_UPGRADING,
    STR_UPDATE_FAILED,

    /* Timezone options */
    STR_TZ_BEIJING,
    STR_TZ_LONDON,
    STR_TZ_NEW_YORK,
    STR_TZ_TOKYO,

    /* Podcast — splash / card / tab bar */
    STR_WELCOME,
    STR_EPISODES,
    STR_TAB_NETWORK,
    STR_TAB_LOCAL,
    STR_TAB_PLAY,
    STR_TAB_ME,

    /* Podcast — network */
    STR_NO_NETWORK,
    STR_OPEN_SETTINGS,
    STR_LOADING,
    STR_REQUEST_TIMED_OUT,
    STR_FAILED_TO_LOAD_CONTENT,
    STR_SEARCH_PLACEHOLDER,
    STR_RETRY,
    STR_CATEGORY_OPTIONS,

    /* Podcast — local */
    STR_DELETE_QUOTED,
    STR_DELETE,
    STR_REMOVE_DOWNLOADED,
    STR_GO_TO_NETWORK,
    STR_NO_SD_CARD_LOCAL,
    STR_NO_AUDIO_DOWNLOADED,

    /* Podcast — channel */
    STR_M4A_NEED_DOWNLOAD,
    STR_FAILED_LOAD_EPISODES,
    STR_UNKNOWN,
    STR_DOWNLOAD_QUEUED,
    STR_DELETE_EPISODES,
    STR_DOWNLOAD,
    STR_CHANNEL_NOT_FOUND,
    STR_ALL,

    /* Podcast — player */
    STR_TITLE,
    STR_TIME,
    STR_REMOVE,
    STR_STOP_BY_TIME,
    STR_MINUTES,
    STR_STOP_BY_TRACKS,
    STR_TRACKS,
    STR_NOTHING_PLAYING,

    /* Podcast — profile */
    STR_WELCOME_SIMPLE,
    STR_LOGIN,
    STR_REGISTER,
    STR_LOGOUT,
    STR_ARE_YOU_SURE_LOGOUT,
    STR_DOWNLOAD_TASK,
    STR_DOWNLOAD_TASK_N,
    STR_NOT_LOGGED_IN,
    STR_TAP_TO_LOGIN,
    STR_PLAY_HOURS,
    STR_DOWNLOADS,

    /* Podcast — search */
    STR_SEARCH,
    STR_SEARCH_PODCASTS,
    STR_CLEAR_SEARCH_HISTORY,
    STR_SEARCH_HISTORY_CLEARED,

    /* Podcast — search results */
    STR_NO_RESULTS_FOUND,
    STR_CHANNELS_N,
    STR_EPISODES_N,
    STR_SEARCH_RESULTS,

    /* Podcast — login */
    STR_NAME,
    STR_ENTER_YOUR_NAME,
    STR_CONFIRM_PASSWORD,
    STR_REENTER_PASSWORD,
    STR_AGREE_TERMS,

    /* Podcast — download task */
    STR_PENDING,
    STR_DONE,
    STR_FAILED,
    STR_ETA,
    STR_STATUS,
    STR_NO_DOWNLOAD_TASKS,
    STR_RESUME,
    STR_PAUSE,
    STR_DELETE_SELECTED_TASKS,
    STR_SEC,
    STR_MIN_EST,

    /* Podcast — settings */
    STR_COUNTRY,
    STR_DOWNLOAD_QUALITY,
    STR_COUNTRY_CHINA,
    STR_COUNTRY_US,
    STR_COUNTRY_UK,
    STR_COUNTRY_JAPAN,
    STR_COUNTRY_GERMANY,
    STR_COUNTRY_FRANCE,
    STR_COUNTRY_CANADA,
    STR_COUNTRY_AUSTRALIA,
    STR_QUALITY_OPTIONS,

    /* Podcast — controller toasts / errors */
    STR_DOWNLOAD_EMPTY,
    STR_NAME_TOO_SHORT,
    STR_PASSWORD_TOO_SHORT,
    STR_PASSWORD_MISMATCH,
    STR_AGREE,
    STR_MEMORY_ALLOC_FAILED,
    STR_FEED_BLACKLISTED,
    STR_SERVER_ERROR,

    /* Podcast — http errors */
    STR_HTTP_INIT_FAILED,
    STR_CONNECT_FAILED,
    STR_OUT_OF_MEMORY,

    /* System — launcher */
    STR_APPLICATIONS,
    STR_NO_APPS,
    STR_ALL_APPS_HIDDEN,
    STR_EXIT_RETURN,
    STR_EXIT,
    STR_APP_FALLBACK,

    /* Podcast — player (action button) */
    STR_PLAY,

    /* Player — app / files / playback */
    STR_APP_PLAYER,
    STR_TAB_FILES,
    STR_PLAYER_EMPTY_DIR,
    STR_PLAYER_FAILED_TO_PLAY,
    STR_PLAYER_NO_AUDIO,

    /* Settings — General (icon size) */
    STR_ICON_SIZE,
    STR_ICON_SIZE_OPTIONS,

    STR_COUNT
};
