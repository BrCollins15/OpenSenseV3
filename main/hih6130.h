#pragma once
#include "opensense.h"
#include "esp_err.h"

/* HIH6130/6131 humidity and temperature driver.
 * values[0] = humidity %RH
 * values[1] = temperature °C
 * Address: 0x27 (fixed, not configurable)
 *
 * Protocol:
 *   1. Bare address write (zero data bytes) — triggers measurement
 *   2. Wait 37ms for conversion
 *   3. Read 4 bytes — [status|hum_hi, hum_lo, temp_hi, temp_lo]
 *      bits 15:14 of byte 0 = status (0=ok, 1=stale, 2=cmd, 3=diag)
 *      bits 13:0  of bytes 0-1 = humidity raw (14-bit)
 *      bits 15:2  of bytes 2-3 = temperature raw (14-bit, shift right 2)
 */
esp_err_t hih6130_read(uint8_t port,
                       const sensor_config_t *cfg,
                       sensor_reading_t      *out);
