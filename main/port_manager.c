#include "port_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG             "PORT_MGR"
#define STAGE_SETTLE_MS  1

typedef struct { bool enabled; bool voltage_5v; } port_state_t;

static port_state_t s_state[5]    = {0};
static bool         s_initialized = false;

static void gpio_out(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin), .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type  = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, 0);
}

static inline int pwrmux_gpio(uint8_t p) { return (p == 3) ? PWRMUX1_SW_GPIO : PWRMUX2_SW_GPIO; }
static inline int sw_gpio    (uint8_t p) { return (p == 3) ? SW1_GPIO        : SW2_GPIO;        }
static inline int tran_gpio  (uint8_t p) { return (p == 3) ? TRAN1_EN_GPIO   : TRAN2_EN_GPIO;   }

/* ── Voltage switching sequences ────────────────────────────────────────────
 * NOTE: The following sequence functions are preserved for future hardware
 * development. Voltage switching between 3.3V and 5V on ports 4 and 5
 * is disabled in this firmware version. The GPIO infrastructure and
 * sequencing logic remain intact for future developers to enable when
 * hardware support is confirmed. */

static void sequence_to_5v(uint8_t port)
{
    /* Future voltage switching development:
     * Sequences PWRMUX → SW → TRAN to bring up 5V path.
     * PWRMUX is hardware-inverted: LOW = 5V output. */
    (void)port;
    /* gpio_set_level(pwrmux_gpio(port), 0); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS));
     * gpio_set_level(sw_gpio(port),     1); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS));
     * gpio_set_level(tran_gpio(port),   1); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS)); */
}

static void sequence_to_low(uint8_t port)
{
    /* Future voltage switching development:
     * Reverse sequence: TRAN → SW → PWRMUX to return to safe 3.3V state.
     * PWRMUX is hardware-inverted: HIGH = 3.3V output. */
    (void)port;
    /* gpio_set_level(tran_gpio(port),   0); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS));
     * gpio_set_level(sw_gpio(port),     0); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS));
     * gpio_set_level(pwrmux_gpio(port), 1); vTaskDelay(pdMS_TO_TICKS(STAGE_SETTLE_MS)); */
}

esp_err_t port_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    /* All switching GPIOs initialised to safe default state.
     * PWRMUX HIGH = 3.3V, SW and TRAN LOW = signal path disconnected. */
    gpio_out(PWRMUX1_SW_GPIO); gpio_set_level(PWRMUX1_SW_GPIO, 1);
    gpio_out(SW1_GPIO);
    gpio_out(TRAN1_EN_GPIO);

    gpio_out(PWRMUX2_SW_GPIO); gpio_set_level(PWRMUX2_SW_GPIO, 1);
    gpio_out(SW2_GPIO);
    gpio_out(TRAN2_EN_GPIO);

    s_state[3] = (port_state_t){ .enabled = false, .voltage_5v = false };
    s_state[4] = (port_state_t){ .enabled = false, .voltage_5v = false };

    s_initialized = true;
    ESP_LOGI(TAG, "Initialised — PWRMUX HIGH (3.3V safe), SW/TRAN LOW");
    return ESP_OK;
}

void port_manager_apply_configs(const sensor_config_t *configs)
{
    for (int p = 3; p <= 4; p++) {
        const sensor_config_t *c = &configs[p];
        if (c->enabled) port_manager_update((uint8_t)p, true, c->voltage_5v);
    }
}

void port_manager_update(uint8_t port, bool enabled, bool voltage_5v)
{
    if (port < 3 || port > 4) return;

    port_state_t *cur = &s_state[port];
    bool currently_5v = cur->enabled && cur->voltage_5v;
    bool currently_on = cur->enabled;

    if (!enabled && !currently_on) return;
    if (enabled == cur->enabled && (!enabled || voltage_5v == cur->voltage_5v)) return;

    if (!enabled) {
        if (currently_5v) sequence_to_low(port);
        cur->enabled = false; cur->voltage_5v = false;
        ESP_LOGI(TAG, "Port %d: disabled", port);
        return;
    }

    if (!currently_on) {
        /* Voltage switching disabled — ports 4 and 5 operate at 3.3V.
         * sequence_to_5v() preserved for future hardware development. */
        if (voltage_5v) sequence_to_5v(port);
        else ESP_LOGI(TAG, "Port %d: enabled", port);
    } else if (currently_5v && !voltage_5v) {
        sequence_to_low(port);
    } else if (!currently_5v && voltage_5v) {
        sequence_to_5v(port);
    }

    cur->enabled    = enabled;
    cur->voltage_5v = voltage_5v;
}
