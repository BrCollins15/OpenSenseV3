#pragma once
#include "opensense.h"
#include "esp_err.h"

/* values[0]=temp°C  values[1]=humidity%RH
 * Addresses: 0x44 (ADDR low, default)  0x45 (ADDR high) */
esp_err_t sht30_read(uint8_t port,
                     const sensor_config_t *cfg,
                     sensor_reading_t      *out);
