#pragma once

/* What each board's backend provides to cyd_display_common.c. Callers have
   already checked that the rectangle lies inside the display. */

#include <stdint.h>

#include "esp_err.h"

esp_err_t cyd_backend_fill_rect(int x, int y, int width, int height, uint16_t color);

/* Draws row-major RGB565 pixels in native byte order. */
esp_err_t cyd_backend_blit(int x, int y, int width, int height, const uint16_t *pixels);
