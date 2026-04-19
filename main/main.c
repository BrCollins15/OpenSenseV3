#include "opensense.h"
#include "i2c_bus.h"
#include "tca9548a.h"
#include "generic_sensor.h"
#include "port_manager.h"
#include "sd_card.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "sensor_task.h"
#include "logger_task.h"
#include "telemetry_task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

#define TAG "MAIN"

sensor_state_t g_sensor_state = {0};
QueueHandle_t  g_log_queue    = NULL;

/* Blinks IO22 at 1 Hz — confirms firmware is running */
static void heartbeat_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << 22),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    while (true) {
        gpio_set_level(22, 1); vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(22, 0); vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, " OpenSense v3.5  —  MainBoard V3");
    ESP_LOGI(TAG, "===========================================");

    /* NVS must be first */
    esp_err_t nvs_ret = nvs_config_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        nvs_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "NVS corrupted — erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_config_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }

    g_sensor_state.mutex = xSemaphoreCreateMutex();
    configASSERT(g_sensor_state.mutex);

    /* Queue entries by value — no malloc/free in the hot path */
    g_log_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_entry_t));
    configASSERT(g_log_queue);

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(port_manager_init());

    /* Deselect all MUX channels to avoid bus contention at boot */
    tca9548a_deselect_all();

    ESP_LOGI(TAG, "Loading sensor configs from NVS...");
    if (xSemaphoreTake(g_sensor_state.mutex, portMAX_DELAY) == pdTRUE) {
        for (int p = 0; p < MAX_SENSORS; p++) {
            nvs_config_load_sensor(p, &g_sensor_state.configs[p]);
            if (g_sensor_state.configs[p].enabled) {
                ESP_LOGI(TAG, "  Port %d: '%s'  addr=0x%02X  regs=%d", p,
                         g_sensor_state.configs[p].name,
                         g_sensor_state.configs[p].i2c_addr,
                         g_sensor_state.configs[p].num_regs);
            } else {
                ESP_LOGI(TAG, "  Port %d: disabled", p);
            }
        }
        xSemaphoreGive(g_sensor_state.mutex);
    }

    /* Run power-up sequences for any enabled switching ports */
    port_manager_apply_configs(g_sensor_state.configs);

    /* SD card failures are non-fatal */
    if (sd_card_init() == ESP_OK) {
        uint32_t session_id = 0;
        nvs_config_increment_boot_count(&session_id);
        if (sd_card_open_log(session_id) != ESP_OK)
            ESP_LOGW(TAG, "SD card mounted but log file could not be created");
    } else {
        ESP_LOGW(TAG, "SD card unavailable — logging disabled for this session");
    }

    ESP_LOGI(TAG, "Starting WiFi AP...");
    ESP_ERROR_CHECK(wifi_mgr_start());

    httpd_handle_t http_server = NULL;
    ESP_ERROR_CHECK(web_server_start(&http_server));
    telemetry_set_server(http_server);

    /* Task priorities: sensor p4, telemetry p3, logger p2, heartbeat p1
     * httpd runs at p5 (IDF default) — all user tasks stay below TCP/IP (p18) and WiFi (p23) */
    xTaskCreate(sensor_task,    "sensor",    5120, NULL, 4, NULL);
    xTaskCreate(telemetry_task, "telemetry", 8192, NULL, 3, NULL);
    xTaskCreate(logger_task,    "logger",    5120, NULL, 2, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 1, NULL);

    ESP_LOGI(TAG, "All tasks started — connect to '%s' and open http://192.168.4.1", WIFI_AP_SSID);
}
