#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WMG_MODE_AP = 0,
} wifi_mgr_mode_t;

typedef struct {
    wifi_mgr_mode_t mode;
    bool            connected;
    char            ssid[64];
    char            ip[20];
} wifi_mgr_status_t;

/* Starts AP hotspot — always runs in AP mode */
esp_err_t wifi_mgr_start(void);
void      wifi_mgr_get_status(wifi_mgr_status_t *status);
