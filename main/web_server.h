#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* HTTP endpoints:
 *   GET/POST /api/config           — sensor port configuration
 *   GET/POST /api/wifi             — WiFi credentials
 *   GET      /api/sdcard           — SD card status
 *   GET      /api/sdcard/download  — stream current log CSV
 *   POST     /api/sdcard/erase     — delete all files, reset boot counter
 *   GET      /ws                   — WebSocket (live telemetry) */

esp_err_t web_server_start(httpd_handle_t *out_handle);
void      web_server_stop(httpd_handle_t handle);
