#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Only one channel should be active at a time.
 * Always call tca9548a_deselect_all() after reading a port. */
esp_err_t tca9548a_select_channel(uint8_t channel);  /* enable one channel (0-7) */
esp_err_t tca9548a_deselect_all(void);                /* write 0x00 — disables all */
