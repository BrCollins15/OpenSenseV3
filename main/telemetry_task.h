#pragma once
#include "opensense.h"
#include "esp_http_server.h"

/* Pushes sensor JSON to all WebSocket clients at TELEMETRY_RATE_HZ.
 * Sends are dispatched via httpd_queue_work() — the only thread-safe
 * context for httpd_ws_send_frame_async.
 * Stack: 8192  Priority: 3 */
void telemetry_set_server(httpd_handle_t server);
void telemetry_task(void *arg);
