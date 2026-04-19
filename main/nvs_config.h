#pragma once
#include "opensense.h"
#include "esp_err.h"
#include <stdint.h>

/* All keys live in the "opensense" NVS namespace:
 *   "sensor_0".."sensor_4" — sensor_config_t blobs
 *   "boot_count"           — uint32 incremented each boot (log session ID) */

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_save_sensor(uint8_t port, const sensor_config_t *cfg);
esp_err_t nvs_config_load_sensor(uint8_t port, sensor_config_t *cfg);
esp_err_t nvs_config_get_boot_count(uint32_t *count);
esp_err_t nvs_config_increment_boot_count(uint32_t *new_count);
esp_err_t nvs_config_reset_boot_count(void);
