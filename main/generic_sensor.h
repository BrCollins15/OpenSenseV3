#pragma once
#include "opensense.h"
#include "esp_err.h"

/* Read all configured registers on one port.
 * Power for ports 3 & 4 must already be applied by port_manager before calling. */
esp_err_t generic_sensor_read(uint8_t port,
                               const sensor_config_t *cfg,
                               sensor_reading_t       *out);
