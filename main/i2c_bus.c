#include "i2c_bus.h"
#include "opensense.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG            "I2C_BUS"
/* 10ms timeout: a non-responding device won't answer faster with more time;
 * short timeout limits CPU starvation when a port fails */
#define I2C_TIMEOUT_MS  10

/* Bus recovery — toggles SCL 9 times to unstick a slave holding SDA low.
 * Called automatically before driver install. */
static void i2c_bus_recover(void)
{
    /* Configure SDA and SCL as open-drain outputs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << OPENSENSE_I2C_SDA) | (1ULL << OPENSENSE_I2C_SCL),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* Send 9 SCL pulses — enough to clock out any stuck byte */
    gpio_set_level(OPENSENSE_I2C_SDA, 1);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(OPENSENSE_I2C_SCL, 0);
        vTaskDelay(1);
        gpio_set_level(OPENSENSE_I2C_SCL, 1);
        vTaskDelay(1);
        /* If SDA is released by the slave we can stop early */
        if (gpio_get_level(OPENSENSE_I2C_SDA)) break;
    }

    /* Send STOP condition to cleanly terminate any open transaction */
    gpio_set_level(OPENSENSE_I2C_SDA, 0);
    vTaskDelay(1);
    gpio_set_level(OPENSENSE_I2C_SCL, 1);
    vTaskDelay(1);
    gpio_set_level(OPENSENSE_I2C_SDA, 1);
    vTaskDelay(1);

    ESP_LOGI(TAG, "I2C bus recovery complete — SDA=%d",
             gpio_get_level(OPENSENSE_I2C_SDA));
}

esp_err_t i2c_bus_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = OPENSENSE_I2C_SDA,
        .scl_io_num       = OPENSENSE_I2C_SCL,
        /* Internal pull-ups disabled — external pull-ups on sensor boards
         * are used instead. This prevents the ESP32 3.3V pull-ups from
         * fighting the 5V pull-ups on level-translated ports (4 & 5),
         * which caused idle SDA/SCL to sit at ~3.7V instead of 5V. */
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,
        .master.clk_speed = OPENSENSE_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(OPENSENSE_I2C_PORT, &conf);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "param_config failed: %s", esp_err_to_name(ret)); return ret; }

    ret = i2c_driver_install(OPENSENSE_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "driver_install failed: %s", esp_err_to_name(ret)); return ret; }

    ESP_LOGI(TAG, "I2C master ready — SDA=%d  SCL=%d  @%d Hz",
             OPENSENSE_I2C_SDA, OPENSENSE_I2C_SCL, OPENSENSE_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t i2c_bus_write_byte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data)
{
    uint8_t buf[2] = { reg_addr, data };
    return i2c_master_write_to_device(OPENSENSE_I2C_PORT, dev_addr,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t i2c_bus_write(uint8_t dev_addr, uint8_t reg_addr,
                        const uint8_t *buf, size_t len)
{
    if (len > 4) return ESP_ERR_INVALID_ARG;
    /* Prepend reg_addr so the wire frame is [addr W][reg][buf...] */
    uint8_t frame[5] = { reg_addr };
    for (size_t i = 0; i < len; i++) frame[1 + i] = buf[i];
    return i2c_master_write_to_device(OPENSENSE_I2C_PORT, dev_addr,
                                      frame, 1 + len,
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t i2c_bus_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *buf, size_t len)
{
    /* Sends reg_addr then issues a repeated-start before reading */
    return i2c_master_write_read_device(OPENSENSE_I2C_PORT, dev_addr,
                                        &reg_addr, 1,
                                        buf, len,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t i2c_bus_read_direct(uint8_t dev_addr, uint8_t *buf, size_t len)
{
    /* [addr R] then clock out N bytes — no register address written first */
    return i2c_master_read_from_device(OPENSENSE_I2C_PORT, dev_addr,
                                       buf, len,
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t i2c_bus_write_cmd(uint8_t dev_addr, uint8_t cmd)
{
    return i2c_master_write_to_device(OPENSENSE_I2C_PORT, dev_addr,
                                      &cmd, 1,
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

esp_err_t i2c_bus_trigger(uint8_t dev_addr)
{
    /* Send bare address write with no data bytes: [addr W][STOP]
     * Must use cmd_link API — i2c_master_write_to_device always
     * sends at least one byte which interferes with trigger-only devices. */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    /* ack_en = false — HIH6130 may not ACK the trigger write cleanly.
     * We send the address and STOP regardless of ACK to fire the measurement. */
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, false);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(OPENSENSE_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

void i2c_bus_scan(const char *label)
{
    /* Must use cmd_link API for address probing — i2c_master_write_to_device()
     * with a NULL buffer triggers a driver error on the legacy driver. */
    ESP_LOGI(TAG, "=== I2C scan: %s ===", label);
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(OPENSENSE_I2C_PORT, cmd,
                                             pdMS_TO_TICKS(I2C_TIMEOUT_MS));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) { ESP_LOGI(TAG, "  [0x%02X] FOUND", addr); found++; }
    }

    if (found == 0) ESP_LOGW(TAG, "  No devices found — check wiring and pull-ups");
    ESP_LOGI(TAG, "=== scan done: %d device(s) ===", found);
}
