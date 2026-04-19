#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "opensense.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define TAG "WIFI_MGR"

static wifi_mgr_status_t s_status = {0};

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        /* MACSTR can't be used in ESP_LOGI format strings on IDF v5.5+ */
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ev->mac[0], ev->mac[1], ev->mac[2],
                 ev->mac[3], ev->mac[4], ev->mac[5]);
        ESP_LOGI(TAG, "Client connected — MAC: %s", mac_str);
    }
    else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Client disconnected");
    }
}

esp_err_t wifi_mgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Disable power save — prevents TX latency spikes on soft-AP */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_event_handler_instance_t inst_wifi;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &inst_wifi));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = (uint8_t)strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    /* HT20 avoids adjacent-channel interference on a soft-AP */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_status.ip,  sizeof(s_status.ip),  "192.168.4.1");
    strncpy(s_status.ssid, WIFI_AP_SSID, sizeof(s_status.ssid) - 1);
    s_status.mode      = WMG_MODE_AP;
    s_status.connected = true;

    ESP_LOGI(TAG, "AP ready — SSID: '%s'  IP: http://192.168.4.1", WIFI_AP_SSID);
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_mgr_status_t *status) { *status = s_status; }
