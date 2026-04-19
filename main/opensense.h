#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

/* ── I2C master bus ─────────────────────────────────────────────────────── */
#define OPENSENSE_I2C_PORT    I2C_NUM_0
#define OPENSENSE_I2C_SDA     32
#define OPENSENSE_I2C_SCL     33
#define OPENSENSE_I2C_FREQ_HZ 100000

/* TCA9548A — A0/A1/A2 all pulled to GND → base address 0x70 */
#define TCA9548A_ADDR 0x70

/* ── Switching sensor paths — ports 3 & 4 ──────────────────────────────── */
/* Each port has three ICs that must be sequenced in order:
 *   1. PWRMUX (TPS2110APWR)   — establish the supply rail first
 *   2. SW    (TS3A24157DGSR)  — connect signal lines once supply is stable
 *   3. TRAN  (NTB0102DP)      — bring translated signals online last
 * Reverse order for power-down.
 * NOTE: PWRMUX logic is hardware-inverted — HIGH=3.3V, LOW=5V */

/* Port 3 */
#define PWRMUX1_SW_GPIO  13   /* HIGH=3.3V (safe)  LOW=5V  */
#define SW1_GPIO          5   /* HIGH=5V path  LOW=3.3V path */
#define TRAN1_EN_GPIO    12   /* HIGH=on  LOW=off */

/* Port 4 */
#define PWRMUX2_SW_GPIO  16   /* HIGH=3.3V (safe)  LOW=5V  */
#define SW2_GPIO         14
#define TRAN2_EN_GPIO    15

/* ── SD card SPI ────────────────────────────────────────────────────────── */
#define SD_MISO_GPIO    17
#define SD_SCK_GPIO     18
#define SD_MOSI_GPIO    19
#define SD_CS_GPIO      21
#define SD_MOUNT_POINT  "/sdcard"
#define SD_SPI_HOST     SPI2_HOST

/* ── Sampling constants ─────────────────────────────────────────────────── */
#define MAX_SENSORS         5
#define MAX_REGS_PER_SENSOR 4
#define SENSOR_NAME_LEN     32
#define UNIT_LEN            16
#define SAMPLE_RATE_HZ      10
#define SAMPLE_PERIOD_MS    (1000 / SAMPLE_RATE_HZ)
#define TELEMETRY_RATE_HZ   2
#define TELEMETRY_PERIOD_MS (1000 / TELEMETRY_RATE_HZ)
#define LOG_QUEUE_DEPTH     30

/* ── WiFi AP ────────────────────────────────────────────────────────────── */
#define WIFI_AP_SSID       "OpenSenseV3"
#define WIFI_AP_PASS       "opensense"
#define WIFI_AP_CHANNEL    6
#define WIFI_AP_MAX_CONN   4

/* ══════════════════════════════════════════════════════════════════════════
   DATA STRUCTURES
══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  reg_addr;
    uint8_t  byte_len;
    bool     is_signed;
    bool     big_endian;
    float    scale;
    float    offset;
    char     expr[64];
    char     label[SENSOR_NAME_LEN];
    char     unit[UNIT_LEN];
    bool     write_before_read;
    uint8_t  write_len;
    uint8_t  write_buf[4];
    uint32_t delay_us;
    uint8_t  data_shift;
    uint32_t data_mask;
    uint8_t  valid_bits;
    bool     direct_read;
    uint8_t  merge_with;   /* index of reg to OR with (255 = disabled) */
    uint8_t  merge_shift;  /* left-shift this raw value before OR with paired reg */
} reg_config_t;

typedef enum {
    SENSOR_TYPE_I2C   = 0,
    SENSOR_TYPE_ADC   = 1,
    SENSOR_TYPE_SHT30 = 2,
    SENSOR_TYPE_BME280 = 3,
    SENSOR_TYPE_HIH6130= 4,   /* Honeywell HIH6130/6131 humidity + temperature */
} sensor_type_t;

typedef struct {
    bool          enabled;
    sensor_type_t sensor_type;
    uint8_t       i2c_addr;
    bool          voltage_5v;
    char          name[SENSOR_NAME_LEN];
    uint8_t       num_regs;
    reg_config_t  regs[MAX_REGS_PER_SENSOR];
} sensor_config_t;

typedef struct {
    float   values[MAX_REGS_PER_SENSOR];
    bool    valid;
    int64_t timestamp_us;
} sensor_reading_t;

typedef struct {
    sensor_config_t   configs[MAX_SENSORS];
    sensor_reading_t  readings[MAX_SENSORS];
    SemaphoreHandle_t mutex;
    uint32_t          config_version;
} sensor_state_t;

typedef struct {
    int64_t          timestamp_us;
    sensor_reading_t readings[MAX_SENSORS];
} log_entry_t;

extern sensor_state_t g_sensor_state;
extern QueueHandle_t  g_log_queue;
