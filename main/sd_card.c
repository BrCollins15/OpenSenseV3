#include "sd_card.h"
#include "nvs_config.h"
#include "logger_task.h"
#include "opensense.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <inttypes.h>

#define TAG                "SD_CARD"
/* fsync every 5 rows (500ms at 10Hz) — balances data safety vs write wear */
#define FLUSH_INTERVAL_ROWS  5

static sdmmc_card_t *s_card         = NULL;
static FILE         *s_log_fp       = NULL;
static bool          s_mounted      = false;
static uint32_t      s_row_ctr      = 0;
static char          s_log_path[80] = {0};

esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Initialising SD card on SPI host %d...", SD_SPI_HOST);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO, .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCK_GPIO,  .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    /* ESP_ERR_INVALID_STATE = SPI bus already initialised — not fatal */
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_GPIO;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* never auto-format — would destroy user data */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — card absent or not FAT32?", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_card_open_log(uint32_t session_id)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char path[80];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/log_%04" PRIu32 ".csv", session_id);
    s_log_fp  = fopen(path, "w");
    s_row_ctr = 0;

    if (!s_log_fp) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        s_log_path[0] = '\0';
        return ESP_FAIL;
    }
    strncpy(s_log_path, path, sizeof(s_log_path) - 1);
    ESP_LOGI(TAG, "Log file opened: %s", path);
    return ESP_OK;
}

esp_err_t sd_card_write_header(const char *header)
{
    if (!s_log_fp) return ESP_ERR_INVALID_STATE;
    if (fprintf(s_log_fp, "%s\n", header) < 0 || fflush(s_log_fp) != 0) {
        ESP_LOGW(TAG, "Header write failed — marking unmounted");
        fclose(s_log_fp); s_log_fp = NULL; s_mounted = false; s_log_path[0] = '\0';
        return ESP_FAIL;
    }
    /* fsync commits the directory entry — fflush alone leaves file size as 0
     * on the card if power is lost before the next sync */
    fsync(fileno(s_log_fp));
    return ESP_OK;
}

esp_err_t sd_card_write_row(const char *row)
{
    if (!s_log_fp) return ESP_ERR_INVALID_STATE;

    if (fputs(row, s_log_fp) == EOF || fputc('\n', s_log_fp) == EOF) {
        ESP_LOGW(TAG, "Write failed — marking unmounted");
        fclose(s_log_fp); s_log_fp = NULL; s_mounted = false; s_log_path[0] = '\0';
        return ESP_FAIL;
    }

    if (++s_row_ctr % FLUSH_INTERVAL_ROWS == 0) {
        if (fflush(s_log_fp) != 0) {
            ESP_LOGW(TAG, "Flush failed — marking unmounted");
            fclose(s_log_fp); s_log_fp = NULL; s_mounted = false; s_log_path[0] = '\0';
            return ESP_FAIL;
        }
        fsync(fileno(s_log_fp));   /* keep FAT directory entry up to date */
    }
    return ESP_OK;
}

void sd_card_close_log(void)
{
    if (!s_log_fp) return;
    fflush(s_log_fp); fclose(s_log_fp); s_log_fp = NULL;
    ESP_LOGI(TAG, "Log closed — %" PRIu32 " rows written to %s", s_row_ctr, s_log_path);
    s_row_ctr = 0; s_log_path[0] = '\0';
}

bool        sd_card_is_mounted(void) { return s_mounted; }
bool        sd_card_is_logging(void) { return s_log_fp != NULL; }
const char *sd_card_log_path(void)   { return s_log_path; }
uint32_t    sd_card_row_count(void)  { return s_row_ctr; }

void sd_card_get_status(sd_status_t *out)
{
    out->mounted      = s_mounted;
    out->logging      = (s_log_fp != NULL);
    out->rows_written = s_row_ctr;
    strncpy(out->log_path, s_log_path, sizeof(out->log_path) - 1);
    out->log_path[sizeof(out->log_path) - 1] = '\0';
    out->card_size_mb = (s_card && s_mounted)
        ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024ULL * 1024ULL)
        : 0;
}

esp_err_t sd_card_erase_and_reset(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    sd_card_close_log();

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) { ESP_LOGE(TAG, "opendir failed"); return ESP_FAIL; }

    struct dirent *ent;
    char path[264];
    int  deleted = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", ent->d_name);
            if (remove(path) == 0) { ESP_LOGI(TAG, "Deleted %s", path); deleted++; }
            else                   ESP_LOGW(TAG, "Failed to delete %s", path);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Erase complete — %d file(s) deleted", deleted);

    nvs_config_reset_boot_count();
    logger_reset_header();
    return sd_card_open_log(1);
}

esp_err_t sd_card_try_remount(void)
{
    if (s_mounted) return ESP_OK;   /* already mounted — nothing to do */

    ESP_LOGI(TAG, "Attempting SD card remount...");

    /* Re-init the card — spi_bus_initialize returns ESP_ERR_INVALID_STATE
     * on second call which sd_card_init already handles gracefully */
    esp_err_t ret = sd_card_init();
    if (ret != ESP_OK) {
        return ret;   /* card still absent or unreadable */
    }

    /* Card is back — open a new log file with the next session ID */
    uint32_t session_id = 0;
    nvs_config_increment_boot_count(&session_id);
    ret = sd_card_open_log(session_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD remounted but could not open log file");
    }
    return ret;
}
