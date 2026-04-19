#include "bme280.h"
#include "i2c_bus.h"
#include "tca9548a.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "BME280"

/* ── Register addresses ─────────────────────────────────────────────────── */
#define REG_CALIB_00   0x88   /* calibration block 1 (26 bytes) */
#define REG_CALIB_26   0xE1   /* calibration block 2 (7 bytes)  */
#define REG_CHIP_ID    0xD0   /* should read 0x60 */
#define REG_RESET      0xE0
#define REG_CTRL_HUM   0xF2
#define REG_STATUS     0xF3
#define REG_CTRL_MEAS  0xF4
#define REG_CONFIG     0xF5
#define REG_DATA       0xF7   /* 8 bytes: press[3] temp[3] hum[2] */

/* ── Calibration data ───────────────────────────────────────────────────── */
typedef struct {
    uint16_t dig_T1; int16_t dig_T2; int16_t dig_T3;
    uint16_t dig_P1; int16_t dig_P2; int16_t dig_P3;
    int16_t  dig_P4; int16_t dig_P5; int16_t dig_P6;
    int16_t  dig_P7; int16_t dig_P8; int16_t dig_P9;
    uint8_t  dig_H1; int16_t dig_H2; uint8_t dig_H3;
    int16_t  dig_H4; int16_t dig_H5; int8_t  dig_H6;
} bme280_calib_t;

/* Per-port driver state */
typedef struct {
    bool           initialized;
    uint8_t        i2c_addr;    /* tracked to detect address changes */
    bme280_calib_t calib;
} bme280_state_t;

static bme280_state_t s_state[MAX_SENSORS];

/* ── Calibration parse helpers ──────────────────────────────────────────── */
static uint16_t u16le(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static int16_t  s16le(const uint8_t *b) { return (int16_t) u16le(b); }

static esp_err_t read_calibration(uint8_t addr, bme280_calib_t *cal)
{
    uint8_t b1[26] = {0};
    esp_err_t ret = i2c_bus_read(addr, REG_CALIB_00, b1, sizeof(b1));
    if (ret != ESP_OK) return ret;

    cal->dig_T1 = u16le(b1 +  0); cal->dig_T2 = s16le(b1 +  2);
    cal->dig_T3 = s16le(b1 +  4); cal->dig_P1 = u16le(b1 +  6);
    cal->dig_P2 = s16le(b1 +  8); cal->dig_P3 = s16le(b1 + 10);
    cal->dig_P4 = s16le(b1 + 12); cal->dig_P5 = s16le(b1 + 14);
    cal->dig_P6 = s16le(b1 + 16); cal->dig_P7 = s16le(b1 + 18);
    cal->dig_P8 = s16le(b1 + 20); cal->dig_P9 = s16le(b1 + 22);
    cal->dig_H1 = b1[25];

    uint8_t b2[7] = {0};
    ret = i2c_bus_read(addr, REG_CALIB_26, b2, sizeof(b2));
    if (ret != ESP_OK) return ret;

    cal->dig_H2 = s16le(b2 + 0);
    cal->dig_H3 = b2[2];
    /* dig_H4/H5 share a nibble in register 0xE5 */
    cal->dig_H4 = (int16_t)((b2[3] << 4) | (b2[4] & 0x0F));
    cal->dig_H5 = (int16_t)((b2[4] >> 4) | (b2[5] << 4));
    cal->dig_H6 = (int8_t)b2[6];

    return ESP_OK;
}

/* ── Bosch compensation formulas (datasheet §4.2.3) ────────────────────── */
static float compensate_temperature(int32_t adc_T, const bme280_calib_t *cal,
                                    int32_t *t_fine)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)cal->dig_T1 << 1)))
                    * ((int32_t)cal->dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)cal->dig_T1)
                      * ((adc_T >> 4) - (int32_t)cal->dig_T1)) >> 12)
                    * (int32_t)cal->dig_T3) >> 14;
    *t_fine = var1 + var2;
    return (float)((*t_fine * 5 + 128) >> 8) / 100.0f;
}

static float compensate_pressure(int32_t adc_P, int32_t t_fine,
                                  const bme280_calib_t *cal)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)cal->dig_P6;
    var2 += (var1 * (int64_t)cal->dig_P5) << 17;
    var2 += (int64_t)cal->dig_P4 << 35;
    var1  = ((var1 * var1 * (int64_t)cal->dig_P3) >> 8)
           + ((var1 * (int64_t)cal->dig_P2) << 12);
    var1  = (((int64_t)1 << 47) + var1) * (int64_t)cal->dig_P1 >> 33;
    if (var1 == 0) return 0.0f;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)cal->dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)cal->dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)cal->dig_P7 << 4);
    return (float)p / 25600.0f;   /* Pa → hPa */
}

static float compensate_humidity(int32_t adc_H, int32_t t_fine,
                                  const bme280_calib_t *cal)
{
    int32_t v = t_fine - 76800;
    v = (((((adc_H << 14)
            - ((int32_t)cal->dig_H4 << 20)
            - ((int32_t)cal->dig_H5 * v)) + 16384) >> 15)
         * (((((((v * (int32_t)cal->dig_H6) >> 10)
                * (((v * (int32_t)cal->dig_H3) >> 11) + 32768)) >> 10)
              + 2097152) * (int32_t)cal->dig_H2 + 8192) >> 14));
    v -= (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)cal->dig_H1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    return (float)(v >> 12) / 1024.0f;
}

/* ── Init ───────────────────────────────────────────────────────────────── */
static esp_err_t bme280_init(uint8_t port, uint8_t addr)
{
    bme280_state_t *st = &s_state[port];

    uint8_t chip_id = 0;
    esp_err_t ret = i2c_bus_read(addr, REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port %d: chip ID read failed: %s", port, esp_err_to_name(ret));
        return ret;
    }
    if (chip_id != 0x60) {
        /* Some clones report 0x58 (BMP280) — continue anyway */
        ESP_LOGW(TAG, "Port %d: unexpected chip ID 0x%02X", port, chip_id);
    }

    i2c_bus_write_byte(addr, REG_RESET, 0xB6);   /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(5));

    ret = read_calibration(addr, &st->calib);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Port %d: calibration read failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    /* ctrl_hum MUST be written before ctrl_meas — datasheet p.26 */
    i2c_bus_write_byte(addr, REG_CTRL_HUM,  0x01);  /* humidity 1x oversampling */
    i2c_bus_write_byte(addr, REG_CONFIG,    0x20);  /* standby 62.5ms, filter off */
    i2c_bus_write_byte(addr, REG_CTRL_MEAS, 0x27);  /* temp 1x, press 1x, normal mode */

    vTaskDelay(pdMS_TO_TICKS(15));   /* wait for first measurement */

    st->initialized = true;
    st->i2c_addr    = addr;
    ESP_LOGI(TAG, "Port %d: BME280 @ 0x%02X ready", port, addr);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */
void bme280_reset(uint8_t port)
{
    if (port < MAX_SENSORS) s_state[port].initialized = false;
}

esp_err_t bme280_read(uint8_t port,
                      const sensor_config_t *cfg,
                      sensor_reading_t      *out)
{
    memset(out, 0, sizeof(*out));
    out->valid        = false;
    out->timestamp_us = esp_timer_get_time();

    if (!cfg->enabled) return ESP_OK;

    bme280_state_t *st = &s_state[port];

    esp_err_t ret = tca9548a_select_channel(port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Port %d: MUX select failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    /* Re-init if first access or address changed */
    if (!st->initialized || st->i2c_addr != cfg->i2c_addr) {
        ret = bme280_init(port, cfg->i2c_addr);
        if (ret != ESP_OK) { tca9548a_deselect_all(); return ret; }
    }

    /* 8 bytes from 0xF7: press[3] temp[3] hum[2] */
    uint8_t buf[8] = {0};
    ret = i2c_bus_read(cfg->i2c_addr, REG_DATA, buf, sizeof(buf));
    tca9548a_deselect_all();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Port %d: data read failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    /* Reconstruct 20-bit ADC values from the packed register bytes */
    int32_t adc_P = (int32_t)(((uint32_t)buf[0] << 12) | ((uint32_t)buf[1] << 4) | ((uint32_t)buf[2] >> 4));
    int32_t adc_T = (int32_t)(((uint32_t)buf[3] << 12) | ((uint32_t)buf[4] << 4) | ((uint32_t)buf[5] >> 4));
    int32_t adc_H = (int32_t)(((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7]);

    /* Temperature must be compensated first — it produces t_fine used by pressure and humidity */
    int32_t t_fine;
    out->values[0] = compensate_temperature(adc_T, &st->calib, &t_fine);
    out->values[1] = compensate_pressure(adc_P, t_fine, &st->calib);
    out->values[2] = compensate_humidity(adc_H, t_fine, &st->calib);
    out->valid     = true;

    ESP_LOGV(TAG, "Port %d: T=%.2f°C  P=%.2f hPa  RH=%.1f%%",
             port, out->values[0], out->values[1], out->values[2]);
    return ESP_OK;
}
