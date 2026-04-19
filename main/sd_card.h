#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* SD card via SPI2 (MISO=17 SCK=18 MOSI=19 CS=21).
 * One log file per boot: /sdcard/log_NNNN.csv  (NNNN = NVS boot counter)
 * fflush every FLUSH_INTERVAL_ROWS rows; fsync updates the FAT directory entry. */

esp_err_t   sd_card_init(void);
esp_err_t   sd_card_open_log(uint32_t session_id);
esp_err_t   sd_card_write_header(const char *header);
esp_err_t   sd_card_write_row(const char *row);
void        sd_card_close_log(void);

bool        sd_card_is_mounted(void);
bool        sd_card_is_logging(void);
const char *sd_card_log_path(void);
uint32_t    sd_card_row_count(void);

typedef struct {
    bool     mounted;
    bool     logging;
    char     log_path[80];
    uint32_t rows_written;
    uint64_t card_size_mb;
} sd_status_t;

void      sd_card_get_status(sd_status_t *out);
esp_err_t sd_card_erase_and_reset(void);

/* Attempt to remount the SD card and open a new log file.
 * Safe to call repeatedly — returns ESP_OK immediately if already mounted. */
esp_err_t sd_card_try_remount(void);
