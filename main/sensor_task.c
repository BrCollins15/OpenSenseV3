#include "sensor_task.h"
#include "generic_sensor.h"
#include "sht30.h"
#include "bme280.h"
#include "hih6130.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

#define TAG              "SENSOR_TASK"
#define STATS_INTERVAL_S  10

void sensor_task(void *arg)
{
    ESP_LOGI(TAG, "Started — %d Hz  (%d ms period)", SAMPLE_RATE_HZ, SAMPLE_PERIOD_MS);

    /* Config cache — only refreshed when config_version increments,
     * saving a 1800-byte memcpy on cycles where nothing changed */
    static sensor_config_t s_cfg[MAX_SENSORS];
    static uint32_t        s_cfg_ver = UINT32_MAX;

    /* Timing stats — logged every STATS_INTERVAL_S seconds */
    int64_t stat_i2c_min = INT64_MAX, stat_i2c_max = 0, stat_i2c_sum = 0;
    int64_t stat_cycle_min = INT64_MAX, stat_cycle_max = 0, stat_cycle_sum = 0;
    uint32_t stat_n = 0;
    int64_t  stat_next_log = esp_timer_get_time() + (int64_t)STATS_INTERVAL_S * 1000000LL;

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        int64_t cycle_start = esp_timer_get_time();

        /* ── 1. Refresh config cache if version changed ──────────────────── */
        {
            uint32_t ver;
            if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                ver = g_sensor_state.config_version;
                if (ver != s_cfg_ver) {
                    memcpy(s_cfg, g_sensor_state.configs, sizeof(s_cfg));
                    s_cfg_ver = ver;
                    ESP_LOGI(TAG, "Config cache refreshed (v%" PRIu32 ")", ver);
                    /* Force BME280 re-init so it picks up any address change */
                    for (int p = 0; p < MAX_SENSORS; p++) bme280_reset(p);
                }
                xSemaphoreGive(g_sensor_state.mutex);
            } else {
                ESP_LOGW(TAG, "Mutex timeout — skipping cycle");
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
                continue;
            }
        }

        /* ── 2. Read all ports (I2C runs outside the mutex) ─────────────── */
        sensor_reading_t fresh[MAX_SENSORS];
        int64_t i2c_start = esp_timer_get_time();

        for (int p = 0; p < MAX_SENSORS; p++) {
            switch (s_cfg[p].sensor_type) {
                case SENSOR_TYPE_SHT30:  sht30_read(p, &s_cfg[p], &fresh[p]);  break;
                case SENSOR_TYPE_BME280:  bme280_read(p, &s_cfg[p], &fresh[p]);  break;
                case SENSOR_TYPE_HIH6130: hih6130_read(p, &s_cfg[p], &fresh[p]); break;
                default:                 generic_sensor_read(p, &s_cfg[p], &fresh[p]); break;
            }
        }
        int64_t i2c_us = esp_timer_get_time() - i2c_start;

        /* ── 3. Publish to shared state ──────────────────────────────────── */
        if (xSemaphoreTake(g_sensor_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            memcpy(g_sensor_state.readings, fresh, sizeof(fresh));
            xSemaphoreGive(g_sensor_state.mutex);
        }

        /* ── 4. Queue log entry BY VALUE — zero heap allocation ─────────── */
        log_entry_t entry;
        entry.timestamp_us = fresh[0].timestamp_us;
        memcpy(entry.readings, fresh, sizeof(fresh));
        if (xQueueSend(g_log_queue, &entry, 0) != pdTRUE) {
            ESP_LOGD(TAG, "Log queue full — sample dropped");
        }

        /* ── 5. Timing stats ─────────────────────────────────────────────── */
        int64_t cycle_us = esp_timer_get_time() - cycle_start;

        if (i2c_us    < stat_i2c_min)   stat_i2c_min   = i2c_us;
        if (i2c_us    > stat_i2c_max)   stat_i2c_max   = i2c_us;
        stat_i2c_sum += i2c_us;

        if (cycle_us  < stat_cycle_min) stat_cycle_min = cycle_us;
        if (cycle_us  > stat_cycle_max) stat_cycle_max = cycle_us;
        stat_cycle_sum += cycle_us;
        stat_n++;

        int64_t now = esp_timer_get_time();
        if (now >= stat_next_log && stat_n > 0) {
            ESP_LOGI(TAG,
                "Timing over %" PRIu32 " cycles — "
                "I2C: min/avg/max = %" PRId64 "/%" PRId64 "/%" PRId64 " µs  |  "
                "Cycle: min/avg/max = %" PRId64 "/%" PRId64 "/%" PRId64 " µs  (budget %d ms)",
                stat_n,
                stat_i2c_min,   stat_i2c_sum / stat_n,   stat_i2c_max,
                stat_cycle_min, stat_cycle_sum / stat_n,  stat_cycle_max,
                SAMPLE_PERIOD_MS);

            if (stat_cycle_max > (int64_t)SAMPLE_PERIOD_MS * 1000)
                ESP_LOGW(TAG, "Cycle overrun! max=%" PRId64 " µs > %d ms budget",
                         stat_cycle_max, SAMPLE_PERIOD_MS);

            stat_i2c_min = INT64_MAX; stat_i2c_max = 0; stat_i2c_sum = 0;
            stat_cycle_min = INT64_MAX; stat_cycle_max = 0; stat_cycle_sum = 0;
            stat_n = 0;
            stat_next_log = now + (int64_t)STATS_INTERVAL_S * 1000000LL;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}
