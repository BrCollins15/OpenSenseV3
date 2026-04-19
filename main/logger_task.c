#include "logger_task.h"
#include "sd_card.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define TAG              "LOGGER"
#define ROW_BUF_LEN       768
#define HDR_BUF_LEN      1536
#define REMOUNT_INTERVAL_US  3000000LL   /* try remount every 3 s when card absent */

static char s_hdr_buf[HDR_BUF_LEN];
static char s_row_buf[ROW_BUF_LEN];
static volatile bool s_reset_header = false;

static void build_header(const sensor_config_t *cfgs, char *buf, size_t buf_len)
{
    int pos = 0;
    pos += snprintf(buf + pos, buf_len - pos, "elapsed_time");

    for (int p = 0; p < MAX_SENSORS; p++) {
        const sensor_config_t *c = &cfgs[p];
        if (!c->enabled || c->num_regs == 0) continue;
        const char *pname = (c->name[0] != '\0') ? c->name : "port";

        for (int r = 0; r < c->num_regs && pos < (int)buf_len - 2; r++) {
            const reg_config_t *rc = &c->regs[r];
            const char *lbl = (rc->label[0] != '\0') ? rc->label : "val";
            if (rc->unit[0] != '\0')
                pos += snprintf(buf + pos, buf_len - pos, ",p%d_%s_%s(%s)", p, pname, lbl, rc->unit);
            else
                pos += snprintf(buf + pos, buf_len - pos, ",p%d_%s_%s",     p, pname, lbl);
        }
    }
}

static void build_row(const log_entry_t *entry,
                      const sensor_config_t *cfgs,
                      char *buf, size_t buf_len)
{
    int pos = 0;
    int64_t us   = entry->timestamp_us;
    int     ms   = (int)((us / 1000LL)       % 1000);
    int     secs = (int)((us / 1000000LL)    % 60);
    int     mins = (int)((us / 60000000LL)   % 60);
    int     hrs  = (int) (us / 3600000000LL);
    pos += snprintf(buf + pos, buf_len - pos, "%02d:%02d:%02d.%03d", hrs, mins, secs, ms);

    for (int p = 0; p < MAX_SENSORS; p++) {
        const sensor_config_t  *c = &cfgs[p];
        const sensor_reading_t *r = &entry->readings[p];
        if (!c->enabled || c->num_regs == 0) continue;
        for (int i = 0; i < c->num_regs && pos < (int)buf_len - 2; i++) {
            /* Empty field on failed read preserves column alignment */
            if (r->valid) pos += snprintf(buf + pos, buf_len - pos, ",%.6f", r->values[i]);
            else          pos += snprintf(buf + pos, buf_len - pos, ",");
        }
    }
}

void logger_task(void *arg)
{
    ESP_LOGI(TAG, "Started");

    static sensor_config_t s_cfg[MAX_SENSORS];
    static uint32_t        s_cfg_ver = UINT32_MAX;
    bool    header_written  = false;
    int64_t last_remount_us = 0;
    log_entry_t entry;

    while (true) {
        if (xQueueReceive(g_log_queue, &entry, portMAX_DELAY) != pdTRUE) continue;

        /* ── SD card not mounted ─────────────────────────────────────────
         * Attempt a remount every REMOUNT_INTERVAL_US.
         * This catches both hot-plug insertion and recovery after removal. */
        if (!sd_card_is_mounted()) {
            int64_t now = esp_timer_get_time();
            if (now - last_remount_us >= REMOUNT_INTERVAL_US) {
                last_remount_us = now;
                if (sd_card_try_remount() == ESP_OK) {
                    ESP_LOGI(TAG, "SD card detected and mounted — logging resumed");
                    header_written = false;   /* write fresh header into new log file */
                } else {
                    ESP_LOGD(TAG, "SD card still absent");
                }
            }
            continue;   /* nothing to write yet */
        }

        /* ── Refresh config cache if version changed ─────────────────── */
        {
            uint32_t ver;
            if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                ver = g_sensor_state.config_version;
                if (ver != s_cfg_ver) {
                    memcpy(s_cfg, g_sensor_state.configs, sizeof(s_cfg));
                    s_cfg_ver = ver;
                    header_written = false;
                    ESP_LOGI(TAG, "Config updated (v%" PRIu32 ") — resetting CSV header", ver);
                }
                xSemaphoreGive(g_sensor_state.mutex);
            }
        }

        if (s_reset_header) { header_written = false; s_reset_header = false; }

        /* ── Write header if needed ──────────────────────────────────── */
        if (!header_written) {
            build_header(s_cfg, s_hdr_buf, sizeof(s_hdr_buf));
            if (sd_card_write_header(s_hdr_buf) == ESP_OK) {
                ESP_LOGI(TAG, "CSV header written");
                header_written = true;
            } else {
                ESP_LOGE(TAG, "Header write failed — card may have been removed");
                continue;
            }
        }

        /* ── Write data row ──────────────────────────────────────────── */
        build_row(&entry, s_cfg, s_row_buf, sizeof(s_row_buf));
        if (sd_card_write_row(s_row_buf) != ESP_OK) {
            /* sd_card_write_row already marked card unmounted on failure */
            ESP_LOGW(TAG, "Row write failed — SD card removed");
            header_written = false;
        }
    }
}

void logger_reset_header(void) { s_reset_header = true; }
