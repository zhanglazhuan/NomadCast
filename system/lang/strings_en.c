/* English string table. Index-aligned with lang_strings.h STR_* enum. */
#include "lang_strings.h"

const char *const strings_en[STR_COUNT] = {
    /* App display names */
    "Podcast",
    "Settings",

    /* Settings — main menu */
    "General",
    "WiFi",
    "Storage",
    "Update",
    "About",

    /* Settings — General */
    "Timezone",
    "Language",
    "24-Hour Format",
    "Sleep Timeout",
    "Auto Power Off",
    "Factory Reset",
    "Erase all settings and downloaded data?\nThe device will restart.",
    "Erase and Restart",
    "Cancel",
    "Never\n1 Minute\n2 Minutes\n5 Minutes\n10 Minutes\n15 Minutes\n30 Minutes\n60 Minutes",
    "Never\n5 Minutes\n10 Minutes\n15 Minutes\n30 Minutes\n60 Minutes",

    /* Settings — WiFi */
    "WiFi connected",
    "Connection failed",
    "Scan for Networks",
    "Scanning...",
    "Available Networks (%d)",

    /* Settings — WiFi Connect */
    "Connect",
    "Enter Password",
    "Password",
    "Show Password",

    /* Settings — Storage */
    "No SD Card Insert ~",
    "Storage Usage",
    "Clean Storage",

    /* Settings — Update */
    "Auto Update",
    "Check for Update",
    "OTA Update",
    "Battery must be at least 30%.\nDo not power off during the update.",
    "Confirm",
    "Already up-to-date",
    "Released: %s",
    "OTA Now",
    "URL not configured",
    "Server unreachable",
    "Invalid server response",
    "No network connection",

    /* Settings — About */
    "Device Name",
    "Device ID",
    "Firmware Ver",
    "SDK",
    "Hardware",
    "LVGL",

    /* Settings — OTA status */
    "OTA Status",
    "Upgrading. #FF0000 Do not power off.#",
    "Update failed",

    /* Timezone options */
    "UTC+8 Beijing",
    "UTC+0 London",
    "UTC-5 New York",
    "UTC+9 Tokyo",

    /* Podcast — splash / card / tab bar */
    "Welcome to\nNomadCast",
    "%d episodes",
    "Network",
    "Local",
    "Play",
    "Me",

    /* Podcast — network */
    "No network.\nPlease connect WiFi in Settings.",
    "Open Settings",
    "Loading",
    "Request timed out. Please retry.",
    "Failed to load content",
    "Search...",
    "Retry",
    "All\nNews\nTechnology\nHumanities\nLifestyle\nEducation\nOthers",

    /* Podcast — local */
    "Delete \"%s\"?",
    "Delete",
    "This will remove all downloaded\naudio files for this channel.",
    "Go to Network",
    "No SD card inserted, no local content.",
    "No audio downloaded yet. Please go to Network to download.",

    /* Podcast — channel */
    "M4A must be downloaded before playing",
    "Failed to load episodes.\nCheck network connection.",
    "Unknown",
    "Download queued",
    "Delete %d episodes?",
    "Download",
    "Channel not found",
    "ALL",

    /* Podcast — player */
    "Title",
    "Time",
    "Remove",
    "Stop by time",
    "minutes",
    "Stop by tracks",
    "tracks",
    "Nothing playing",

    /* Podcast — profile */
    "Welcome",
    "Login",
    "Register",
    "Logout",
    "Are you sure logout?",
    "Download Task",
    "Download Task(%d)",
    "Not logged in",
    "Tap to login",
    "Play Hours",
    "Downloads",

    /* Podcast — search */
    "Search",
    "Search podcasts...",
    "Clear search history",
    "Search history cleared",

    /* Podcast — search results */
    "No results found",
    "Channels (%d)",
    "Episodes (%d)",
    "Search Results",

    /* Podcast — login */
    "Name",
    "Enter your name",
    "Confirm Password",
    "Re-enter password",
    "I agree to the Terms of Service",

    /* Podcast — download task */
    "Pending",
    "Done",
    "Failed",
    "ETA",
    "Status",
    "No download tasks",
    "Resume",
    "Pause",
    "Delete selected tasks?\n(Downloaded files are kept;\nunfinished ones are removed.)",
    "%d sec",
    "%d min",

    /* Podcast — settings */
    "Country",
    "Download Quality",
    "China",
    "United States",
    "United Kingdom",
    "Japan",
    "Germany",
    "France",
    "Canada",
    "Australia",
    "Low (64kbps)\nMedium (128kbps)\nHigh (320kbps)",

    /* Podcast — controller toasts / errors */
    "This download is empty or corrupt",
    "Name too short",
    "Password too short",
    "Mismatch",
    "Agree",
    "Memory allocation failed",
    "Feed unavailable (blacklisted)",
    "Server error",

    /* Podcast — http errors */
    "Failed to init HTTP client",
    "Connect failed: %s",
    "Out of memory (%d bytes)",

    /* System — launcher */
    "Applications",
    "No apps installed",
    "All apps are hidden",
    "Exit \"%s\" and\nreturn to launcher?",
    "Exit",
    "App",

    /* Podcast — player (action button) */
    "Play",

    /* Player — app / files / playback */
    "Player",
    "Files",
    "Empty folder",
    "Failed to play",
    "No playable audio on SD card",

    /* Settings — General (icon size) */
    "Icon Size",
    "Medium\nLarge",

    /* Radio — app / stations / playback */
    "Radio",
    "Stations",
    "LIVE",
    "Failed to play",
    "No stations",

    /* Alarm — app / list / edit / ringing */
    "Alarm",
    "No alarms",
    "Add alarm",
    "Edit alarm",
    "Save",
    "Delete",
    "Alarm",
    "Dismiss",
    "Snooze",
    "Repeat",
    "Once",
    "Daily",
    "Weekdays",
    "Will ring again in 5 minutes",
};
