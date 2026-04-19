#include "sht30.h"
#include "i2c_bus.h"
#include "tca9548a.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "SHT30"

/* CRC-8 — poly 0x31, init 0xFF (Sensirion standard) */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

esp_err_t sht30_read(uint8_t port,
                     const sensor_config_t *cfg,
                     sensor_reading_t      *out)
{
    memset(out, 0, sizeof(*out));
    out->valid        = false;
    out->timestamp_us = esp_timer_get_time();

    if (!cfg->enabled) return ESP_OK;

    esp_err_t ret = tca9548a_select_channel(port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Port %d: MUX select failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    /* Single-shot high repeatability command: [addr W][0x2C][0x06] */
    const uint8_t cmd_byte = 0x06;
    ret = i2c_bus_write(cfg->i2c_addr, 0x2C, &cmd_byte, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port %d: measurement command failed: %s", port, esp_err_to_name(ret));
        tca9548a_deselect_all();
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(20));   /* high repeatability conversion: up to 15.5ms */

    /* Response: T_MSB T_LSB T_CRC H_MSB H_LSB H_CRC */
    uint8_t buf[6] = {0};
    ret = i2c_bus_read_direct(cfg->i2c_addr, buf, sizeof(buf));
    tca9548a_deselect_all();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port %d: read failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    if (crc8(buf,   2) != buf[2]) { ESP_LOGW(TAG, "Port %d: T CRC fail", port); return ESP_OK; }
    if (crc8(buf+3, 2) != buf[5]) { ESP_LOGW(TAG, "Port %d: H CRC fail", port); return ESP_OK; }

    uint16_t t_raw = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t h_raw = ((uint16_t)buf[3] << 8) | buf[4];

    out->values[0] = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    out->values[1] = 100.0f * (float)h_raw / 65535.0f;
    out->valid     = true;

    ESP_LOGV(TAG, "Port %d: T=%.2f°C  RH=%.1f%%", port, out->values[0], out->values[1]);
    return ESP_OK;
}
