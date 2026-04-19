#include "telemetry_task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define TAG          "TELEMETRY"
#define MAX_WS_CLIENTS 8
#define JSON_BUF_LEN 3072   /* ~500 chars/sensor × 5 + envelope */

static httpd_handle_t s_server = NULL;

void telemetry_set_server(httpd_handle_t server) { s_server = server; }

/* Escape quotes and backslashes for JSON string values */
static int write_json_str(char *dst, size_t dst_len, const char *src)
{
    int written = 0;
    for (; *src && (size_t)written < dst_len - 2; src++) {
        if (*src == '"' || *src == '\\') {
            if ((size_t)written < dst_len - 3) dst[written++] = '\\';
            else break;
        }
        dst[written++] = *src;
    }
    dst[written] = '\0';
    return written;
}

typedef struct {
    uint8_t payload[JSON_BUF_LEN];
    size_t  len;
} async_send_t;

static async_send_t s_pkt;

/* Failed fds are collected here by ws_send_all_cb, then closed by
 * telemetry_task AFTER the semaphore is given — never from inside the callback.
 * Protected by s_send_sem: no additional lock needed. */
static int    s_failed_fds[MAX_WS_CLIENTS];
static size_t s_failed_count = 0;

/* Binary semaphore — starts GIVEN. telemetry_task takes before writing s_pkt;
 * ws_send_all_cb gives after all sends complete. */
static SemaphoreHandle_t s_send_sem = NULL;

/* Runs inside the httpd task — the only thread-safe context for async WS sends */
static void ws_send_all_cb(void *arg)
{
    (void)arg;
    s_failed_count = 0;

    if (!s_server) goto done;

    size_t num_clients = MAX_WS_CLIENTS;
    int    fds[MAX_WS_CLIENTS];
    if (httpd_get_client_list(s_server, &num_clients, fds) != ESP_OK) goto done;

    for (size_t i = 0; i < num_clients; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;

        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = s_pkt.payload,
            .len     = s_pkt.len,
            .final   = true,
        };

        esp_err_t ret = httpd_ws_send_frame_async(s_server, fds[i], &frame);
        if (ret != ESP_OK) {
            /* Queue for closure — cannot call httpd_sess_trigger_close() here
             * because we're inside the httpd task and it would deadlock on
             * the control socket the httpd task is already waiting on. */
            ESP_LOGW(TAG, "WS fd=%d send failed — queuing for close", fds[i]);
            if (s_failed_count < MAX_WS_CLIENTS)
                s_failed_fds[s_failed_count++] = fds[i];
        }
    }

done:
    xSemaphoreGive(s_send_sem);
}

/* Config cache — refreshed only when config_version changes (configs ~1800 bytes) */
static sensor_config_t s_cfg_cache[MAX_SENSORS];
static uint32_t        s_cfg_ver = UINT32_MAX;

static size_t build_json(char *buf, size_t buf_len)
{
    sensor_reading_t readings[MAX_SENSORS];

    if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout — skipping frame");
        return 0;
    }
    memcpy(readings, g_sensor_state.readings, sizeof(readings));
    uint32_t ver = g_sensor_state.config_version;
    if (ver != s_cfg_ver) {
        memcpy(s_cfg_cache, g_sensor_state.configs, sizeof(s_cfg_cache));
        s_cfg_ver = ver;
    }
    xSemaphoreGive(g_sensor_state.mutex);

    int pos = 0;

    int rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) rssi = ap_info.rssi;

    pos += snprintf(buf + pos, buf_len - pos,
                    "{\"type\":\"data\",\"timestamp_us\":%" PRId64 ",\"rssi\":%d,\"sensors\":[",
                    readings[0].timestamp_us, rssi);

    for (int p = 0; p < MAX_SENSORS && pos < (int)buf_len - 128; p++) {
        if (p > 0) buf[pos++] = ',';

        const sensor_config_t  *c = &s_cfg_cache[p];
        const sensor_reading_t *r = &readings[p];

        char safe_name[SENSOR_NAME_LEN * 2];
        write_json_str(safe_name, sizeof(safe_name), c->name[0] ? c->name : "Unconfigured");

        pos += snprintf(buf + pos, buf_len - pos,
                        "{\"port\":%d,\"name\":\"%s\",\"enabled\":%s,\"valid\":%s,\"readings\":[",
                        p, safe_name,
                        c->enabled ? "true" : "false",
                        r->valid   ? "true" : "false");

        for (int ri = 0; ri < c->num_regs && pos < (int)buf_len - 200; ri++) {
            if (ri > 0) buf[pos++] = ',';
            char safe_label[SENSOR_NAME_LEN * 2], safe_unit[UNIT_LEN * 2];
            write_json_str(safe_label, sizeof(safe_label),
                           c->regs[ri].label[0] ? c->regs[ri].label : "value");
            write_json_str(safe_unit, sizeof(safe_unit), c->regs[ri].unit);
            pos += snprintf(buf + pos, buf_len - pos,
                            "{\"label\":\"%s\",\"value\":%.6g,\"unit\":\"%s\"}",
                            safe_label,
                            (double)(r->valid ? r->values[ri] : 0.0f),
                            safe_unit);
        }
        pos += snprintf(buf + pos, buf_len - pos, "]}");
    }
    pos += snprintf(buf + pos, buf_len - pos, "]}");

    if (pos >= (int)buf_len)
        ESP_LOGW(TAG, "JSON truncated (%d >= %d) — increase JSON_BUF_LEN", pos, (int)buf_len);

    return (size_t)pos;
}

void telemetry_task(void *arg)
{
    s_send_sem = xSemaphoreCreateBinary();
    configASSERT(s_send_sem);
    xSemaphoreGive(s_send_sem);

    ESP_LOGI(TAG, "Started — push %d Hz, sample %d Hz", TELEMETRY_RATE_HZ, SAMPLE_RATE_HZ);

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));

        if (!s_server) continue;

        /* Skip build+send if no WS clients are connected */
        size_t num_clients = MAX_WS_CLIENTS;
        int    fds[MAX_WS_CLIENTS];
        bool   has_ws = false;
        if (httpd_get_client_list(s_server, &num_clients, fds) == ESP_OK) {
            for (size_t i = 0; i < num_clients; i++) {
                if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
                    { has_ws = true; break; }
            }
        }
        if (!has_ws) continue;

        if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Previous send still in progress — skipping cycle");
            continue;
        }

        /* Semaphore acquired — safe to call trigger_close() from this task context */
        for (size_t i = 0; i < s_failed_count; i++) {
            ESP_LOGI(TAG, "Closing dead WS fd=%d", s_failed_fds[i]);
            httpd_sess_trigger_close(s_server, s_failed_fds[i]);
        }
        s_failed_count = 0;

        s_pkt.len = build_json((char *)s_pkt.payload, sizeof(s_pkt.payload));
        if (s_pkt.len == 0) { xSemaphoreGive(s_send_sem); continue; }

        esp_err_t ret = httpd_queue_work(s_server, ws_send_all_cb, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "httpd_queue_work failed: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_send_sem);
        }
        /* On success semaphore stays taken until ws_send_all_cb gives it back */
    }
}
