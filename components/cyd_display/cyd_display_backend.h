#pragma once

/* What each board's backend provides to cyd_display_common.c. Callers have
   already checked that the rectangle lies inside the display. */

#include <stdbool.h>
#include <stdint.h>

#include "cyd_display.h"
#include "esp_err.h"

esp_err_t cyd_backend_fill_rect(int x, int y, int width, int height, uint16_t color);

/* Draws row-major RGB565 pixels in native byte order. */
esp_err_t cyd_backend_blit(int x, int y, int width, int height, const uint16_t *pixels);

/* Whether cyd_touch_set_fast is on. */
bool cyd_backend_touch_fast(void);

/* Hands a tap to the app's tap callback and, unless it took it, queues it. */
void cyd_backend_post_tap(QueueHandle_t queue, const cyd_touch_point_t *point);
