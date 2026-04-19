#pragma once
#include "opensense.h"
#include "esp_err.h"

/* Sole owner of the six switching GPIOs (PWRMUX, SW, TRAN) on ports 3 & 4.
 * No other module may touch these pins.
 *
 * Power sequences:
 *   5V on:   PWRMUX HIGH → SW HIGH → TRAN HIGH
 *   5V off:  TRAN  LOW  → SW  LOW  → PWRMUX LOW   (reverse)
 *   3.3V:    all GPIOs LOW (boot default)
 */

/* Configure all six GPIOs as outputs, drive LOW. Call once at boot. */
esp_err_t port_manager_init(void);

/* Apply saved configs at boot — runs power-up sequences for enabled ports */
void port_manager_apply_configs(const sensor_config_t *configs);

/* Enable/disable a switching port or change its voltage.
 * Runs the minimum necessary GPIO sequence; does nothing if state unchanged.
 * Ports 0-2 are silently ignored (no switching hardware). */
void port_manager_update(uint8_t port, bool enabled, bool voltage_5v);
