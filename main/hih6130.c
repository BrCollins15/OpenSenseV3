#include "hih6130.h"
#include "i2c_bus.h"
#include "tca9548a.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "opensense.h"
#include <string.h>

#define TAG "HIH6130"

static esp_err_t hih_trigger_zero(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(OPENSENSE_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t hih6130_read(uint8_t port,
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

    ret = hih_trigger_zero(cfg->i2c_addr);
    ESP_LOGI(TAG, "Port %d: trigger=%s", port, esp_err_to_name(ret));

    tca9548a_deselect_all();
    vTaskDelay(pdMS_TO_TICKS(200));

    tca9548a_select_channel(port);

    uint8_t buf[4] = {0};
    ret = i2c_bus_read_direct(cfg->i2c_addr, buf, sizeof(buf));
    tca9548a_deselect_all();

    ESP_LOGI(TAG, "Port %d: bytes=%02X %02X %02X %02X",
             port, buf[0], buf[1], buf[2], buf[3]);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port %d: read failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    uint8_t status = (buf[0] >> 6) & 0x03;
    if (status == 1) { ESP_LOGD(TAG, "Port %d: stale", port); return ESP_OK; }
    if (status > 1)  { ESP_LOGW(TAG, "Port %d: bad status=%d", port, status); return ESP_OK; }

    uint16_t raw_hum  = (uint16_t)(((buf[0] & 0x3F) << 8) | buf[1]);
    out->values[0] = (float)raw_hum / 16383.0f * 100.0f;

    uint16_t raw_temp = (uint16_t)(((uint16_t)buf[2] << 6) | (buf[3] >> 2));
    out->values[1] = (float)raw_temp / 16383.0f * 165.0f - 40.0f;

    out->valid = true;

    ESP_LOGI(TAG, "Port %d: RH=%.1f%%  T=%.2f°C", port, out->values[0], out->values[1]);
    return ESP_OK;
}