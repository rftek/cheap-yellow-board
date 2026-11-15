#include "dk_ui.h"

#include "esp_check.h"

void dk_header_gaps(void)
{
    static const int boundaries[] = {4, 7, 14, 18, 22};
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        ESP_ERROR_CHECK(cyd_fill_rect(boundaries[i] * CYD_FONT_WIDTH - 1, 0, 2, CYD_FONT_HEIGHT,
                                      CYD_COLOR_BLACK));
    }
}
