#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t i2c_bus_init(void);

/* [dev W][reg][data] */
esp_err_t i2c_bus_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data);

/* [dev W][reg][buf...] — max 4 bytes payload */
esp_err_t i2c_bus_write(uint8_t dev_addr, uint8_t reg_addr,
                        const uint8_t *buf, size_t len);

/* Write register address then read N bytes (repeated-start) */
esp_err_t i2c_bus_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *buf, size_t len);

/* Read N bytes with no preceding write — for devices with no register pointer */
esp_err_t i2c_bus_read_direct(uint8_t dev_addr, uint8_t *buf, size_t len);

/* Send a single command byte — no register address (used for TCA9548A) */
esp_err_t i2c_bus_write_cmd(uint8_t dev_addr, uint8_t cmd);

/* Bare address write with zero data bytes — [addr W][STOP]
 * Required for sensors like HIH6130 that trigger on address-only write. */
esp_err_t i2c_bus_trigger(uint8_t dev_addr);

/* Probe 0x08-0x77 and log which addresses ACK — diagnostics only */
void i2c_bus_scan(const char *label);
