#include "tone_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"

/* Lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS
#define REFRESH_MS 10

#define ROW_HEADER 0
#define ROW_FILTER 1
#define ROW_FLAT 8
#define SLIDERS 7 /* the last, PITCH, only when the editor asks for it */
#define SLIDER_LABEL_COLUMNS 8
#define SLIDER_X (SLIDER_LABEL_COLUMNS * FW + 8)
#define SLIDER_WIDTH (CYD_LCD_WIDTH - SLIDER_X - 12)

#define COLOR_TRACK 0x2104 /* a slider's empty part */
#define COLOR_ZERO 0x7BEF  /* a bipolar slider's middle mark */
#define COLOR_DISABLED CYD_COLOR_BUTTON_DISABLED

typedef struct {
    int column;
    int width;
} span_t;

static const span_t PREV_SPAN = {0, 3};
static const span_t TITLE_SPAN = {3, 16};
static const span_t NEXT_SPAN = {19, 3};
static const span_t BACK_SPAN = {22, 4};
static const span_t FILTER_LABEL_SPAN = {0, 6};
static const span_t FILTER_SPANS[TONE_FILTERS] = {{6, 5}, {11, 5}, {16, 5}, {21, 5}};
static const char *const FILTER_NAMES[TONE_FILTERS] = {"OFF", "LP", "HP", "BP"};
static const span_t FLAT_SPAN = {0, 8};
/* With PITCH on row 8, FLAT moves into the header beside a shorter title. */
static const span_t PITCH_TITLE_SPAN = {3, 11};
static const span_t HEADER_FLAT_SPAN = {14, 5};

typedef struct {
    int row;
    int min;
    int max;
    bool bipolar; /* drawn from the middle, as a boost or a cut */
} slider_t;

static const slider_t sliders[SLIDERS] = {
    {2, 0, TONE_CUTOFF_MAX, false},
    {3, 0, TONE_RESONANCE_MAX, false},
    {4, TONE_EQ_MIN, TONE_EQ_MAX, true},
    {5, TONE_EQ_MIN, TONE_EQ_MAX, true},
    {6, TONE_EQ_MIN, TONE_EQ_MAX, true},
    {7, TONE_LEVEL_MIN, TONE_LEVEL_MAX, true},
    {ROW_FLAT, TONE_PITCH_MIN, TONE_PITCH_MAX, true},
};

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

static void clear_row(row_t *row, uint16_t foreground)
{
    memset(row->text, ' ', COLUMNS);
    row->text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row->fg[column] = foreground;
        row->bg[column] = CYD_COLOR_BLACK;
    }
}

/* Writes text centered in a span. */
static void put_span(row_t *row, span_t span, const char *label, uint16_t foreground,
                     uint16_t background)
{
    int length = (int)strlen(label);
    if (length > span.width) {
        length = span.width;
    }
    int start = span.column + (span.width - length) / 2;
    for (int column = span.column; column < span.column + span.width; ++column) {
        row->fg[column] = foreground;
        row->bg[column] = background;
    }
    memcpy(row->text + start, label, (size_t)length);
}

static int slider_value(const tone_settings_t *tone, int index)
{
    switch (index) {
    case 0:
        return tone->cutoff;
    case 1:
        return tone->resonance;
    case 2:
        return tone->low;
    case 3:
        return tone->mid;
    case 4:
        return tone->high;
    case 5:
        return tone->level;
    default:
        return tone->pitch;
    }
}

static void set_slider_value(tone_settings_t *tone, int index, int value)
{
    const slider_t *slider = &sliders[index];
    value = value < slider->min ? slider->min : value > slider->max ? slider->max : value;
    switch (index) {
    case 0:
        tone->cutoff = (uint8_t)value;
        break;
    case 1:
        tone->resonance = (uint8_t)value;
        break;
    case 2:
        tone->low = (int8_t)value;
        break;
    case 3:
        tone->mid = (int8_t)value;
        break;
    case 4:
        tone->high = (int8_t)value;
        break;
    case 5:
        tone->level = (int8_t)value;
        break;
    default:
        tone->pitch = (int8_t)value;
        break;
    }
}

static void slider_label(const tone_settings_t *tone, int index, char *label, size_t size)
{
    int value = slider_value(tone, index);
    if (index == 0) {
        float hz = tone_cutoff_hz(value);
        if (hz < 1000.0f) {
            snprintf(label, size, "CUT %4.0f", (double)hz);
        } else if (hz < 10000.0f) {
            snprintf(label, size, "CUT %3.1fk", (double)(hz / 1000.0f));
        } else {
            snprintf(label, size, "CUT %3.0fk", (double)(hz / 1000.0f));
        }
    } else if (index == 1) {
        snprintf(label, size, "RES %4.1f", (double)tone_q(value));
    } else {
        static const char *const names[] = {"LOW", "MID", "HIGH", "LVL", "PTCH"};
        snprintf(label, size, "%-4s%+4d", names[index - 2], value);
    }
}

/* The slider's value for a finger at x. Bipolar sliders snap to 0 near
   the middle, so flat is easy to find. */
static int slider_at(int index, int x)
{
    const slider_t *slider = &sliders[index];
    int span = slider->max - slider->min;
    int value = slider->min + ((x - SLIDER_X) * span + SLIDER_WIDTH / 2) / SLIDER_WIDTH;
    if (slider->bipolar && value != 0 && abs(value) <= span / 24) {
        value = 0;
    }
    return value;
}

static void draw_slider(const tone_settings_t *tone, int index, uint16_t color, bool enabled)
{
    const slider_t *slider = &sliders[index];
    row_t row;
    clear_row(&row, enabled ? CYD_COLOR_WHITE : CYD_COLOR_GRAY);
    char label[SLIDER_LABEL_COLUMNS + 8];
    slider_label(tone, index, label, sizeof(label));
    memcpy(row.text, label, strnlen(label, SLIDER_LABEL_COLUMNS));
    ESP_ERROR_CHECK(cyd_draw_cells(slider->row, row.text, row.fg, row.bg));

    int y = slider->row * FH + FH / 4;
    int height = FH / 2;
    int span = slider->max - slider->min;
    int value = slider_value(tone, index);
    int position = SLIDER_X + (value - slider->min) * SLIDER_WIDTH / span;
    int zero = slider->bipolar ? SLIDER_X + (0 - slider->min) * SLIDER_WIDTH / span : SLIDER_X;
    ESP_ERROR_CHECK(cyd_fill_rect(SLIDER_X, y, SLIDER_WIDTH, height, COLOR_TRACK));
    int from = position < zero ? position : zero;
    int to = position < zero ? zero : position;
    if (to > from) {
        ESP_ERROR_CHECK(cyd_fill_rect(from, y, to - from, height, enabled ? color : COLOR_DISABLED));
    }
    if (slider->bipolar) {
        ESP_ERROR_CHECK(cyd_fill_rect(zero - 1, y - 4, 2, height + 8, COLOR_ZERO));
    }
    int thumb = position - 3 < SLIDER_X ? SLIDER_X : position - 3;
    if (thumb + 6 > SLIDER_X + SLIDER_WIDTH) {
        thumb = SLIDER_X + SLIDER_WIDTH - 6;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(thumb, y - 6, 6, height + 12,
                                  enabled ? CYD_COLOR_WHITE : CYD_COLOR_GRAY));
}

static void draw_buttons(const tone_editor_t *editor, int item)
{
    const tone_settings_t *tone = editor->settings(item, editor->context);
    uint16_t color = editor->color(item, editor->context);
    row_t row;

    clear_row(&row, CYD_COLOR_WHITE);
    char title[32];
    snprintf(title, sizeof(title), "%s TONE", editor->name(item, editor->context));
    put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    if (editor->pitch) {
        put_span(&row, PITCH_TITLE_SPAN, title, CYD_COLOR_BLACK, color);
        put_span(&row, HEADER_FLAT_SPAN, "FLAT", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    } else {
        put_span(&row, TITLE_SPAN, title, CYD_COLOR_BLACK, color);
    }
    put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, BACK_SPAN, "BACK", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(ROW_HEADER, row.text, row.fg, row.bg));
    if (editor->pitch) {
        ESP_ERROR_CHECK(cyd_fill_rect(HEADER_FLAT_SPAN.column * FW - 1, 0, 2, FH, CYD_COLOR_BLACK));
        ESP_ERROR_CHECK(cyd_fill_rect(NEXT_SPAN.column * FW - 1, 0, 2, FH, CYD_COLOR_BLACK));
    }

    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, FILTER_LABEL_SPAN, "FILTER", CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    for (int filter = 0; filter < TONE_FILTERS; ++filter) {
        bool on = tone->filter == filter;
        put_span(&row, FILTER_SPANS[filter], FILTER_NAMES[filter],
                 on ? CYD_COLOR_BLACK : CYD_COLOR_WHITE, on ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    }
    ESP_ERROR_CHECK(cyd_draw_cells(ROW_FILTER, row.text, row.fg, row.bg));
    /* Gaps between the side-by-side filter buttons. */
    for (int filter = 1; filter < TONE_FILTERS; ++filter) {
        ESP_ERROR_CHECK(cyd_fill_rect(FILTER_SPANS[filter].column * FW - 1, ROW_FILTER * FH, 2, FH,
                                      CYD_COLOR_BLACK));
    }

    if (editor->pitch) {
        return; /* row 8 is the PITCH slider */
    }
    clear_row(&row, CYD_COLOR_GRAY);
    put_span(&row, FLAT_SPAN, "FLAT", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    static const char hint[] = "Drag a slider to change it";
    size_t room = COLUMNS - 9;
    memcpy(row.text + 9, hint, strlen(hint) < room ? strlen(hint) : room);
    ESP_ERROR_CHECK(cyd_draw_cells(ROW_FLAT, row.text, row.fg, row.bg));
}

int tone_editor_run(QueueHandle_t touch_queue, const tone_editor_t *editor, int item)
{
    tone_settings_t drawn_tone = {0};
    int drawn_item = -1;
    bool drawn_filter_on = false;
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    xQueueReset(touch_queue);
    while (true) {
        tone_settings_t *tone = editor->settings(item, editor->context);
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        while (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            int row = point.y / FH;
            int column = point.x / FW;
            if (row >= TONE_EDITOR_BOTTOM_ROW) {
                if (editor->tap != NULL) {
                    editor->tap(&point, item, editor->context);
                }
            } else if (row == ROW_HEADER) {
                if (in_span(BACK_SPAN, column)) {
                    return item;
                }
                if (editor->pitch && in_span(HEADER_FLAT_SPAN, column)) {
                    memset(tone, 0, sizeof(*tone));
                    continue;
                }
                if (in_span(PREV_SPAN, column)) {
                    item = (item + editor->count - 1) % editor->count;
                } else if (in_span(NEXT_SPAN, column)) {
                    item = (item + 1) % editor->count;
                }
                tone = editor->settings(item, editor->context);
            } else if (row == ROW_FILTER) {
                for (int filter = 0; filter < TONE_FILTERS; ++filter) {
                    if (!in_span(FILTER_SPANS[filter], column)) {
                        continue;
                    }
                    if (tone->filter == TONE_FILTER_OFF && filter != TONE_FILTER_OFF &&
                        tone->cutoff == 0 && tone->resonance == 0) {
                        tone->cutoff = TONE_DEFAULT_CUTOFF;
                        tone->resonance = TONE_DEFAULT_RESONANCE;
                    }
                    tone->filter = (uint8_t)filter;
                }
            } else if (row == ROW_FLAT && !editor->pitch && in_span(FLAT_SPAN, column)) {
                memset(tone, 0, sizeof(*tone));
            }
        }
        /* Fingers on the sliders, tapped or dragged. */
        cyd_touch_point_t held[5];
        int count = cyd_touch_held(held, 5);
        for (int i = 0; i < count; ++i) {
            int row = held[i].y / FH;
            for (int index = 0; index < SLIDERS; ++index) {
                if (index == SLIDERS - 1 && !editor->pitch) {
                    continue;
                }
                if (row == sliders[index].row && held[i].x >= SLIDER_X - FW) {
                    set_slider_value(tone, index, slider_at(index, held[i].x));
                }
            }
        }
        bool filter_on = tone->filter != TONE_FILTER_OFF;
        bool new_item = item != drawn_item;
        if (new_item || tone->filter != drawn_tone.filter) {
            draw_buttons(editor, item);
        }
        uint16_t color = editor->color(item, editor->context);
        for (int index = 0; index < SLIDERS; ++index) {
            if (index == SLIDERS - 1 && !editor->pitch) {
                continue;
            }
            bool enabled = index >= 2 || filter_on;
            if (new_item || filter_on != drawn_filter_on ||
                slider_value(tone, index) != slider_value(&drawn_tone, index)) {
                draw_slider(tone, index, color, enabled);
            }
        }
        if (new_item && editor->draw_bottom != NULL) {
            editor->draw_bottom(item, editor->context);
        }
        drawn_tone = *tone;
        drawn_item = item;
        drawn_filter_on = filter_on;
        if (editor->tick != NULL) {
            editor->tick(item, editor->context);
        }
    }
}
