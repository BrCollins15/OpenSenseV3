#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define TAG    "NVS_CFG"
#define NVS_NS "opensense"

static esp_err_t open_rw(nvs_handle_t *h) { return nvs_open(NVS_NS, NVS_READWRITE, h); }
static esp_err_t open_ro(nvs_handle_t *h) { return nvs_open(NVS_NS, NVS_READONLY,  h); }

esp_err_t nvs_config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition dirty — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_config_save_sensor(uint8_t port, const sensor_config_t *cfg)
{
    if (port >= MAX_SENSORS) return ESP_ERR_INVALID_ARG;
    char key[12]; snprintf(key, sizeof(key), "sensor_%d", port);

    nvs_handle_t h;
    esp_err_t ret = open_rw(&h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(h, key, cfg, sizeof(sensor_config_t));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK) ESP_LOGI(TAG, "Port %d config saved", port);
    else               ESP_LOGE(TAG, "Port %d save failed: %s", port, esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_config_load_sensor(uint8_t port, sensor_config_t *cfg)
{
    if (port >= MAX_SENSORS) return ESP_ERR_INVALID_ARG;

    memset(cfg, 0, sizeof(sensor_config_t));
    cfg->enabled = false;
    for (int i = 0; i < MAX_REGS_PER_SENSOR; i++) {
        cfg->regs[i].scale = 1.0f; cfg->regs[i].byte_len = 1;
    }

    char key[12]; snprintf(key, sizeof(key), "sensor_%d", port);

    nvs_handle_t h;
    esp_err_t ret = open_ro(&h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    size_t len = sizeof(sensor_config_t);
    ret = nvs_get_blob(h, key, cfg, &len);
    nvs_close(h);

    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) ESP_LOGE(TAG, "Port %d load failed: %s", port, esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_config_get_boot_count(uint32_t *count)
{
    *count = 0;
    nvs_handle_t h;
    esp_err_t ret = open_ro(&h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    ret = nvs_get_u32(h, "boot_count", count);
    nvs_close(h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *count = 0; return ESP_OK; }
    return ret;
}

esp_err_t nvs_config_increment_boot_count(uint32_t *new_count)
{
    uint32_t count = 0;
    nvs_config_get_boot_count(&count);
    count++;
    nvs_handle_t h;
    esp_err_t ret = open_rw(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u32(h, "boot_count", count);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    *new_count = count;
    ESP_LOGI(TAG, "Boot count: %" PRIu32, count);
    return ret;
}

esp_err_t nvs_config_reset_boot_count(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret)); return ret; }
    ret = nvs_set_u32(h, "boot_count", 0);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Boot count reset — next boot opens log_0001.csv");
    return ret;
}
