#pragma once
#include "opensense.h"
#include "esp_err.h"

/* values[0]=temp°C  values[1]=pressure hPa  values[2]=humidity %RH
 * Addresses: 0x76 (SDO low, default)  0x77 (SDO high)
 * Lazy init on first call per port — reads calibration and starts normal mode. */
esp_err_t bme280_read(uint8_t port,
                      const sensor_config_t *cfg,
                      sensor_reading_t      *out);

/* Force re-init on next read — call when i2c_addr changes */
void bme280_reset(uint8_t port);
