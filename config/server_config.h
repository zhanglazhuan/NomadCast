/*
 * NomadCast — central backend server configuration.
 *
 * The single place to configure the backend server address. Every URL the
 * firmware talks to (podcast API, OTA manifest, log upload, artwork proxy)
 * derives from NOMADCAST_SERVER_BASE.
 *
 * The PC simulator overrides the podcast API independently via
 * -DPODCAST_SERVER=... (see pc_demo/CMakeLists.txt); PODCAST_SERVER is
 * defined in apps/podcast/backend.h as NOMADCAST_SERVER_BASE by default.
 */

#pragma once

/* Backend server: scheme + host + port, no trailing slash. */
#ifndef NOMADCAST_SERVER_BASE
#define NOMADCAST_SERVER_BASE "http://192.168.1.88:5088"
#endif

/* Weather provider — 和风天气 (QWeather), China-accessible + free tier.
 * The API Key is passed as a `key` query param. QWeather now assigns each
 * account a dedicated API Host (console → 设置 → API Host), so the host below
 * is account-specific. Weather (/v7/weather) and city lookup (/geo/v2) share
 * the same host. */
#ifndef QWEATHER_API_KEY
#define QWEATHER_API_KEY "b82e23458d0e41a9bdd619e348f884c2"
#endif

#ifndef QWEATHER_HOST
#define QWEATHER_HOST "https://pt3wt4txdy.re.qweatherapi.com"
#endif
