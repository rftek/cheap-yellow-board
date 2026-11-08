#pragma once

/* The board's microSD card, FAT formatted, mounted at SDCARD_ROOT. It holds
   custom kits (WAVs in KIT_FOLDER/<name>) and exported loops. */

#include <stdbool.h>

#include "esp_err.h"

#define SDCARD_ROOT "/sd"
#define KIT_FOLDER SDCARD_ROOT "/kits"

/* Mounts the card if it isn't mounted yet. ESP_ERR_NOT_FOUND when there is
   no card (or it can't be read); trying again later is fine, so a card can
   be put in after boot. */
esp_err_t sdcard_mount(void);

bool sdcard_mounted(void);
