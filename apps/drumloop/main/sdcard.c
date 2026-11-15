/* The microSD slot of the Waveshare ESP32-S3-Touch-LCD-7: SPI on GPIO 11,
   13 and 12, with chip select on the I/O expander, held low by
   cyd_sd_select (see cyd_display.h). Follows Waveshare's 03_SD_Test. */

#include "sdcard.h"

#include "cyd_display.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_HOST SPI2_HOST
#define SD_PIN_MOSI GPIO_NUM_11
#define SD_PIN_MISO GPIO_NUM_13
#define SD_PIN_SCK GPIO_NUM_12

static const char *TAG = "sdcard";
static sdmmc_card_t *card;

bool sdcard_mounted(void)
{
    return card != NULL;
}

esp_err_t sdcard_mount(void)
{
    if (card != NULL) {
        return ESP_OK;
    }
    esp_err_t result = cyd_sd_select(true);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Can't select the card: %s", esp_err_to_name(result));
        return result;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    result = spi_bus_initialize(SD_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus: %s", esp_err_to_name(result));
        return result;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_HOST;
    slot.gpio_cs = SDSPI_SLOT_NO_CS; /* the expander holds it low */
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    result = esp_vfs_fat_sdspi_mount(SDCARD_ROOT, &host, &slot, &mount, &card);
    if (result != ESP_OK) {
        ESP_LOGI(TAG, "No card: %s", esp_err_to_name(result));
        card = NULL;
        spi_bus_free(SD_HOST);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Mounted %s, %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}
