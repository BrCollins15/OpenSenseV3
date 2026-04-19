#include "generic_sensor.h"
#include "i2c_bus.h"
#include "tca9548a.h"
#include "te.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <string.h>

#define TAG "SENSOR"

/* Assemble multi-byte register value from wire bytes */
static uint32_t bytes_to_raw(const uint8_t *buf, uint8_t len, bool big_endian)
{
    uint32_t raw = 0;
    if (big_endian) {
        for (int i = 0; i < len; i++) raw = (raw << 8) | buf[i];
    } else {
        for (int i = (int)len - 1; i >= 0; i--) raw = (raw << 8) | buf[i];
    }
    return raw;
}

/* Sign-extend using explicit bit width — needed when an ADC result is fewer
 * bits than the register it lives in (e.g. 12-bit result in a 16-bit register) */
static int32_t sign_extend(uint32_t raw, uint8_t valid_bits)
{
    if (valid_bits == 0 || valid_bits >= 32) return (int32_t)raw;
    uint32_t sign_bit = 1u << (valid_bits - 1);
    if (raw & sign_bit) raw |= ~((sign_bit << 1) - 1u);
    return (int32_t)raw;
}

static bool read_sensor_registers(uint8_t port, const sensor_config_t *cfg,
                                   sensor_reading_t *out)
{
    bool all_ok = true;
    int32_t raw_ints[MAX_REGS_PER_SENSOR] = {0};

    for (int i = 0; i < cfg->num_regs; i++) {
        const reg_config_t *r = &cfg->regs[i];

        if (r->byte_len == 0 || r->byte_len > 4) {
            ESP_LOGW(TAG, "Port %d reg[%d]: byte_len %d invalid", port, i, r->byte_len);
            out->values[i] = 0.0f; all_ok = false; continue;
        }

        /* ── Optional write phase ────────────────────────────────────────── */
        if (r->write_before_read) {
            if (r->write_len > 4) {  /* 0 = send reg_addr only, no extra bytes */
                ESP_LOGW(TAG, "Port %d reg[%d]: write_len %d invalid", port, i, r->write_len);
                out->values[i] = 0.0f; all_ok = false; continue;
            }
            esp_err_t wret = i2c_bus_write(cfg->i2c_addr, r->reg_addr,
                                            r->write_buf, r->write_len);
            if (wret != ESP_OK) {
                ESP_LOGW(TAG, "Port %d reg[%d] write fail: %s", port, i, esp_err_to_name(wret));
                out->values[i] = 0.0f; all_ok = false; continue;
            }

            /* esp_rom_delay_us is a busy-spin — only use for sub-1ms delays.
             * Longer delays must yield so TCP/IP and httpd can run. */
            if (r->delay_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS((r->delay_us + 999) / 1000));
            } else if (r->delay_us > 0) {
                esp_rom_delay_us(r->delay_us);
            }
        }

        /* ── Read phase ──────────────────────────────────────────────────── */
        uint8_t buf[4] = {0};
        esp_err_t ret;
        if (r->direct_read) {
            /* No register address — device returns data immediately on address */
            ret = i2c_bus_read_direct(cfg->i2c_addr, buf, r->byte_len);
        } else {
            ret = i2c_bus_read(cfg->i2c_addr, r->reg_addr, buf, r->byte_len);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Port %d reg[%d] read fail: %s", port, i, esp_err_to_name(ret));
            out->values[i] = 0.0f; all_ok = false; continue;
        }

        /* ── Bit extraction ──────────────────────────────────────────────── */
        uint32_t raw_u = bytes_to_raw(buf, r->byte_len, r->big_endian);
        if (r->data_shift > 0) raw_u >>= r->data_shift;
        if (r->data_mask  != 0) raw_u &= r->data_mask;

        int32_t raw;
        if (r->is_signed) {
            /* valid_bits=0 means use byte_len*8 — backwards-compatible */
            uint8_t bits = (r->valid_bits > 0) ? r->valid_bits : (uint8_t)(r->byte_len * 8);
            raw = sign_extend(raw_u, bits);
        } else {
            raw = (int32_t)raw_u;
        }

        /* Store raw integer before scaling — needed for merge pass */
        raw_ints[i] = raw;

        /* ── Expression or scale/offset — applied after merge pass ──────── */
        /* Temporarily store raw as float; will be recomputed after merge */
        out->values[i] = (float)raw;

        ESP_LOGV(TAG, "Port %d reg[%d]: raw=%" PRId32, port, i, raw);
    }

    /* ── Register merge pass ────────────────────────────────────────────
     * Merge happens on raw integers BEFORE scale/offset/expression.
     * High byte raw integer is shifted left by merge_shift bits and
     * ORed with the low byte raw integer to produce the merged integer.
     * Scale/offset/expression is then applied to the merged result. */
    for (int i = 0; i < cfg->num_regs; i++) {
        const reg_config_t *r = &cfg->regs[i];
        if (r->merge_with == 255 || r->merge_shift == 0) continue;
        if (r->merge_with >= cfg->num_regs) continue;
        int j = r->merge_with;
        int32_t merged = (int32_t)(((uint32_t)raw_ints[i] << r->merge_shift) | (uint32_t)raw_ints[j]);
        raw_ints[i] = merged;
        raw_ints[j] = 0;
        out->values[j] = 0.0f;
    }

    /* ── Apply scale/offset/expression after merge ───────────────────── */
    for (int i = 0; i < cfg->num_regs; i++) {
        const reg_config_t *r = &cfg->regs[i];
        /* Skip low-byte registers that were zeroed by merge */
        if (r->merge_with != 255 && r->merge_shift == 0) {
            /* Check if this register is a low byte target of another */
            bool is_low = false;
            for (int k = 0; k < cfg->num_regs; k++) {
                if (cfg->regs[k].merge_with == (uint8_t)i && cfg->regs[k].merge_shift > 0) {
                    is_low = true; break;
                }
            }
            if (is_low) { out->values[i] = 0.0f; continue; }
        }
        int32_t raw = raw_ints[i];
        if (r->expr[0] != '\0') {
            double result;
            te_err_t terr = te_eval(r->expr, (double)raw, &result);
            if (terr == TE_OK) {
                out->values[i] = (float)result;
            } else {
                ESP_LOGW(TAG, "Port %d reg[%d]: expr error %d — using scale/offset", port, i, terr);
                out->values[i] = (float)raw * r->scale + r->offset;
            }
        } else {
            out->values[i] = (float)raw * r->scale + r->offset;
        }
        ESP_LOGV(TAG, "Port %d reg[%d]: merged_raw=%" PRId32 "  val=%.4f %s",
                 port, i, raw, out->values[i], r->unit);
    }

    return all_ok;
}

esp_err_t generic_sensor_read(uint8_t port,
                               const sensor_config_t *cfg,
                               sensor_reading_t       *out)
{
    memset(out, 0, sizeof(*out));
    out->valid        = false;
    out->timestamp_us = esp_timer_get_time();

    if (!cfg->enabled || cfg->num_regs == 0) return ESP_OK;

    esp_err_t ret = tca9548a_select_channel(port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Port %d: MUX select failed: %s", port, esp_err_to_name(ret));
        return ret;
    }

    out->valid = read_sensor_registers(port, cfg, out);
    tca9548a_deselect_all();
    return ESP_OK;
}
