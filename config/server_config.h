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
