#include "tca9548a.h"
#include "opensense.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define TAG "TCA9548A"

esp_err_t tca9548a_select_channel(uint8_t channel)
{
    if (channel > 7) {
        ESP_LOGE(TAG, "Channel %d out of range (0-7)", channel);
        return ESP_ERR_INVALID_ARG;
    }
    /* Control register is a bitmask — exactly one bit high = one active channel */
    uint8_t mask = (uint8_t)(1u << channel);
    esp_err_t ret = i2c_bus_write_cmd(TCA9548A_ADDR, mask);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "select_channel(%d) failed: %s", channel, esp_err_to_name(ret));
    return ret;
}

esp_err_t tca9548a_deselect_all(void)
{
    esp_err_t ret = i2c_bus_write_cmd(TCA9548A_ADDR, 0x00);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "deselect_all failed: %s", esp_err_to_name(ret));
    return ret;
}
