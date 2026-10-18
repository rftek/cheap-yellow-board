/*
 * Second-stage bootloader override, based on ESP-IDF's bootloader_override
 * example. A power-on reset (which includes the EN button) always starts the
 * factory launcher. Any other reset, such as the software restart the
 * launcher performs after a choice, boots the image selected in otadata,
 * exactly like the stock bootloader.
 */
#include <stdbool.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

static const char *TAG = "boot";

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        bootloader_reset();
    }

    int boot_index;
    if (esp_rom_get_reset_reason(0) == RESET_REASON_CHIP_POWER_ON && bs.factory.offset != 0) {
        ESP_LOGI(TAG, "Power-on reset: starting launcher");
        boot_index = FACTORY_INDEX;
    } else {
        boot_index = bootloader_utility_get_selected_boot_partition(&bs);
        if (boot_index == INVALID_INDEX) {
            bootloader_reset();
        }
    }
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
