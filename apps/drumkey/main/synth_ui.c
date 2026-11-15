/* Drum&Key's SYNTH screen: edits the patch synth's patches (psynth.h),
   which the keys and the Piano Roll play as the PATCH voice's styles.

     row 0       PLAY REC - 120 + P1 KEYS MENU   (P1-P4 picks the patch)
     row 1       <  OSC 1  >   SUPERSAW           page (tap: all pages), patch (tap: presets)
     rows 2-7    up to six settings: sliders (drag) or choices (tap)
     row 8       OC-  [ oscilloscope ]  OC+
     rows 9-11   eight test keys, C to C, playing the patch

   Pages: OSC 1, OSC 2, UNISON, VOICE, SUB+NOISE, FILTER 1, FILTER 2, AMP ENV,
   MOD ENV, LFOS, MOD 1-6, MOD 7-8, FX 1, FX 2, RACK, SAMPLES. Tapping the
   page title shows them all at once to jump to any; < > step through them.
   Tapping the patch name opens the presets, by category (BASS, LEAD, PAD,
   KEYS, FX): a tap loads one into the patch, whole or just its oscillators,
   while the test keys stay live to try it. UNDO takes the last load back.
   A choice with more than three options opens a list to pick from.

   The settings rows, the page list, the presets and a choice's list share
   rows 2-7, so the oscilloscope and the test keys are always there.

   While a finger drags a slider the oscilloscope waits, so the screen
   loop follows the finger every 10 ms and the sound moves with it. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "dk_audio.h"
#include "dk_ui.h"
#include "esp_check.h"
#include "music_out.h"
#include "psynth.h"
#include "synth.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS
#define REFRESH_MS 10
#define SCOPE_MS 70

#define PAGE_ROW 1
#define FIRST_PARAM_ROW 2
#define PARAM_ROWS 6
#define SCOPE_ROW 8
#define KEYS_TOP (9 * FH)
#define TEST_KEYS 8
#define KEY_WIDTH (CYD_LCD_WIDTH / TEST_KEYS)
#define LABEL_COLUMNS 9
#define BAR_X (LABEL_COLUMNS * FW + 8)
#define BAR_WIDTH (CYD_LCD_WIDTH - BAR_X - 12)
#define SCOPE_X (3 * FW + 6)
#define SCOPE_WIDTH (20 * FW - 12)
#define MAX_FINGERS 16

/* A mod matrix row: SOURCE, DEST, the amount and its bar. */
#define MATRIX_VALUE_COLUMN 16
#define MATRIX_BAR_X (20 * FW + 4)
#define MATRIX_BAR_WIDTH (CYD_LCD_WIDTH - MATRIX_BAR_X - 12)

#define COLOR_TRACK 0x2104
#define COLOR_ZERO 0x7BEF
#define COLOR_SCOPE_BACK 0x0841
#define COLOR_SCOPE 0x07E0

/* Each group of pages has a color, in the page list and the title. */
#define COLOR_OSC 0xFAD2    /* pink, the PATCH voice's */
#define COLOR_FILTER 0xFC60 /* orange */
#define COLOR_ENV 0x87F0    /* green */
#define COLOR_MOD 0x07FF    /* cyan */
#define COLOR_FX 0xC41F     /* violet */
#define COLOR_RACK 0xFFE0   /* yellow */

static const span_t PLAY_SPAN = DK_PLAY_SPAN;
static const span_t REC_SPAN = DK_REC_SPAN;
static const span_t BPM_DOWN_SPAN = DK_BPM_DOWN_SPAN;
static const span_t BPM_SPAN = DK_BPM_SPAN;
static const span_t BPM_UP_SPAN = DK_BPM_UP_SPAN;
static const span_t SLOT_SPAN = DK_SCREEN_SPAN;
static const span_t SWITCH_SPAN = DK_SWITCH_SPAN;
static const span_t MENU_SPAN = DK_MENU_SPAN;

static const span_t PREV_SPAN = {0, 2};
static const span_t TITLE_SPAN = {2, 9};
static const span_t NEXT_SPAN = {11, 2};
static const span_t NAME_SPAN = {13, 13};
static const span_t CHOICE_SPAN = {LABEL_COLUMNS, COLUMNS - LABEL_COLUMNS};
static const span_t OCT_DOWN_SPAN = {0, 3};
static const span_t OCT_UP_SPAN = {23, 3};

/* ---- The settings ---------------------------------------------------------------- */

static const char *const shape_names[] = {"SAW", "SQUARE", "TRIANGLE", "SINE", "WT BASIC", "WT VOCAL", "WT DIGITAL"};
static const char *const filter_names[] = {"OFF", "LOWPASS", "HIGHPASS", "BANDPASS", "NOTCH", "COMB", "FORMANT"};
static const char *const lfo_names[] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "RANDOM", "GATE", "STAIRS"};
static const char *const sync_names[] = {"FREE", "1/1", "1/2", "1/4", "1/8", "1/16", "1/8T"};
static const char *const source_names[] = {"NONE", "LFO 1", "LFO 2", "MOD ENV", "NOTE", "RANDOM"};
static const char *const dest_names[] = {"NONE", "PITCH", "OSC1 POS", "OSC2 POS", "DETUNE",
                                         "CUTOFF 1", "CUTOFF 2", "RESO 1", "OSC2 LVL", "AMP"};
static const char *const delay_names[] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const char *const sub_names[] = {"SINE", "SQUARE"};
static const char *const routing_names[] = {"SERIES", "PARALLEL"};
static const char *const mode_names[] = {"POLY", "MONO", "LEGATO"};

typedef struct {
    const char *label;
    size_t offset;
    int min, max;
    bool is_signed;
    const char *const *choices; /* NULL for a slider */
} param_t;

#define SLIDER(label, field, min, max) {label, offsetof(psynth_patch_t, field), min, max, false, NULL}
#define SIGNED(label, field, min, max) {label, offsetof(psynth_patch_t, field), min, max, true, NULL}
#define CHOICE(label, field, names) \
    {label, offsetof(psynth_patch_t, field), 0, (int)(sizeof(names) / sizeof(names[0])) - 1, false, names}

typedef enum { PAGE_PARAMS, PAGE_MATRIX, PAGE_RACK, PAGE_SAMPLES } page_kind_t;

typedef struct {
    const char *title;
    int count; /* settings, or mod slots on a matrix page */
    param_t params[PARAM_ROWS];
    page_kind_t kind;
    uint16_t color;
    int first; /* the first mod slot on a matrix page */
} page_t;

#define OSC_PAGE(title, i)                                                                 \
    {title, 6,                                                                             \
     {CHOICE("SHAPE", osc[i].shape, shape_names), SLIDER("POS", osc[i].position, 0, 100), \
      SLIDER("LEVEL", osc[i].level, 0, 100), SIGNED("OCTAVE", osc[i].octave, -3, 3),     \
      SIGNED("SEMI", osc[i].semi, -24, 24), SIGNED("FINE", osc[i].fine, -50, 50)},       \
     PAGE_PARAMS, COLOR_OSC, 0}
#define ENV_PAGE(title, i)                                                                   \
    {title, 4,                                                                               \
     {SLIDER("ATTACK", env[i].attack, 0, 100), SLIDER("DECAY", env[i].decay, 0, 100),     \
      SLIDER("SUSTAIN", env[i].sustain, 0, 100), SLIDER("RELEASE", env[i].release, 0, 100)}, \
     PAGE_PARAMS, COLOR_ENV, 0}

/* In the order the page list shows them, three to a row. */
static const page_t pages[] = {
    OSC_PAGE("OSC 1", 0),
    OSC_PAGE("OSC 2", 1),
    {"UNISON", 4,
     {SLIDER("UNI 1", osc[0].unison, 1, PSYNTH_UNISON_MAX), SLIDER("DET 1", osc[0].detune, 0, 100),
      SLIDER("UNI 2", osc[1].unison, 1, PSYNTH_UNISON_MAX), SLIDER("DET 2", osc[1].detune, 0, 100)},
     PAGE_PARAMS, COLOR_OSC, 0},
    {"VOICE", 3,
     {CHOICE("MODE", mono, mode_names), SLIDER("GLIDE", glide, 0, 100), SLIDER("VOLUME", volume, 0, 100)},
     PAGE_PARAMS, COLOR_OSC, 0},
    {"SUB+NOISE", 4,
     {SLIDER("SUB", sub_level, 0, 100), CHOICE("SUB WAVE", sub_shape, sub_names),
      SLIDER("NOISE", noise_level, 0, 100), SLIDER("COLOR", noise_color, 0, 100)}, PAGE_PARAMS, COLOR_OSC, 0},
    {"FILTER 1", 6,
     {CHOICE("TYPE", filter[0].type, filter_names), SLIDER("CUTOFF", filter[0].cutoff, 0, 100),
      SLIDER("RESO", filter[0].resonance, 0, 100), SLIDER("DRIVE", filter[0].drive, 0, 100),
      SLIDER("KEYTRK", keytrack, 0, 100), CHOICE("ROUTING", routing, routing_names)},
     PAGE_PARAMS, COLOR_FILTER, 0},
    {"FILTER 2", 4,
     {CHOICE("TYPE", filter[1].type, filter_names), SLIDER("CUTOFF", filter[1].cutoff, 0, 100),
      SLIDER("RESO", filter[1].resonance, 0, 100), SLIDER("DRIVE", filter[1].drive, 0, 100)},
     PAGE_PARAMS, COLOR_FILTER, 0},
    ENV_PAGE("AMP ENV", 0),
    ENV_PAGE("MOD ENV", 1),
    {"LFOS", 6,
     {CHOICE("LFO1", lfo[0].shape, lfo_names), SLIDER("L1 RATE", lfo[0].rate, 0, 100),
      CHOICE("L1 SYNC", lfo[0].sync, sync_names), CHOICE("LFO2", lfo[1].shape, lfo_names),
      SLIDER("L2 RATE", lfo[1].rate, 0, 100), CHOICE("L2 SYNC", lfo[1].sync, sync_names)},
     PAGE_PARAMS, COLOR_MOD, 0},
    {"MOD 1-6", 6, {{0}}, PAGE_MATRIX, COLOR_MOD, 0},
    {"MOD 7-8", 2, {{0}}, PAGE_MATRIX, COLOR_MOD, 6},
    {"FX 1", 5,
     {SLIDER("DRIVE", drive, 0, 100), SLIDER("CHORUS", chorus, 0, 100),
      CHOICE("DLY TIME", delay_time, delay_names), SLIDER("DLY FDBK", delay_feedback, 0, 100),
      SLIDER("DLY MIX", delay_mix, 0, 100)}, PAGE_PARAMS, COLOR_FX, 0},
    {"FX 2", 3,
     {SLIDER("REV SIZE", reverb_size, 0, 100), SLIDER("REV MIX", reverb_mix, 0, 100),
      SLIDER("VOLUME", volume, 0, 100)}, PAGE_PARAMS, COLOR_FX, 0},
    {"RACK", 0, {{0}}, PAGE_RACK, COLOR_RACK, 0},
    {"SAMPLES", 0, {{0}}, PAGE_SAMPLES, COLOR_RACK, 0},
};
#define PAGES ((int)(sizeof(pages) / sizeof(pages[0])))

/* The page list: three columns, with PRESETS after the pages. */
#define LIST_COLUMNS 3
_Static_assert(PAGES + 1 <= PARAM_ROWS * LIST_COLUMNS, "the page list has no room for PRESETS");
static const span_t LIST_SPANS[LIST_COLUMNS] = {{0, 9}, {9, 9}, {18, 8}};

/* The presets: categories on the first row, two columns of four, then
   LOAD (whole patch or oscillators only), UNDO, INIT and DONE. */
#define PRESET_ROWS 4
#define PRESETS_SHOWN (2 * PRESET_ROWS)
static const span_t CATEGORY_SPANS[CATEGORIES] = {{0, 5}, {5, 5}, {10, 5}, {15, 5}, {20, 6}};
static const span_t HALF_SPANS[2] = {{0, 13}, {13, 13}};
static const span_t LOAD_SPAN = {0, 10};
static const span_t UNDO_SPAN = {10, 5};
static const span_t INIT_SPAN = {15, 5};
static const span_t DONE_SPAN = {20, 6};

/* A choice's list: its name and BACK, then two columns of five. */
#define CHOICE_ROWS 5
static const span_t BACK_SPAN = {20, 6};

/* A mod matrix row. */
static const span_t SOURCE_SPAN = {0, 7};
static const span_t DEST_SPAN = {7, 9};

/* ---- State ---------------------------------------------------------------------------- */

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

/* What rows 2-7 show over the page's settings. */
typedef enum { OVERLAY_NONE, OVERLAY_PAGES, OVERLAY_PRESETS, OVERLAY_CHOICE } overlay_t;

static int slot;
static int page;
static int base_note = 48; /* C3 */
static overlay_t overlay;
static int category;
static bool osc_only;
static psynth_patch_t undo_patch;
static int undo_slot = -1;
static param_t choice;
static char choice_title[COLUMNS + 1];
static row_t drawn_header;
static row_t drawn_page_row;
static int drawn_values[PARAM_ROWS];
static int drawn_page = -1;
static int drawn_slot = -1;
static int drawn_base = -1;
static TickType_t scope_at;
/* The sample the test keys play on the SAMPLES page, or -1 for the patch. */
static int chosen_sample = -1;
static char sample_status[COLUMNS + 1];
/* What the custom pages and the overlays showed, so they redraw only on a change. */
static uint32_t drawn_custom = 0xFFFFFFFFu;
static bool dragging;

/* Test keys: the note under each finger, or -1. */
static volatile int test_instrument;
static int16_t finger_notes[MAX_FINGERS];

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

static void clear_row(row_t *row, uint16_t fg)
{
    memset(row->text, ' ', COLUMNS);
    row->text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row->fg[column] = fg;
        row->bg[column] = CYD_COLOR_BLACK;
    }
}

static void put_span(row_t *row, span_t span, const char *label, uint16_t fg, uint16_t bg)
{
    int length = (int)strlen(label);
    if (length > span.width) {
        length = span.width;
    }
    int start = span.column + (span.width - length) / 2;
    for (int column = span.column; column < span.column + span.width; ++column) {
        row->fg[column] = fg;
        row->bg[column] = bg;
    }
    memcpy(row->text + start, label, (size_t)length);
}

static void draw_row(int screen_row, const row_t *row)
{
    ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row->text, row->fg, row->bg));
}

/* Thin black lines between cells that touch, after a row of buttons. */
static void draw_dividers(int screen_row, const span_t *spans, int count)
{
    for (int i = 1; i < count; ++i) {
        ESP_ERROR_CHECK(cyd_fill_rect(spans[i].column * FW - 1, screen_row * FH, 2, FH, CYD_COLOR_BLACK));
    }
    ESP_ERROR_CHECK(cyd_fill_rect(0, screen_row * FH, CYD_LCD_WIDTH, 2, CYD_COLOR_BLACK));
}

static int get_value(const param_t *param)
{
    const uint8_t *field = (const uint8_t *)psynth_patch(slot) + param->offset;
    return param->is_signed ? (int)(int8_t)*field : (int)*field;
}

static void set_value(const param_t *param, int value)
{
    value = value < param->min ? param->min : value > param->max ? param->max : value;
    uint8_t *field = (uint8_t *)psynth_patch(slot) + param->offset;
    *field = (uint8_t)(param->is_signed ? (uint8_t)(int8_t)value : (uint8_t)value);
}

/* A matrix slot's settings as params, so the shared code sets them. */
static param_t matrix_param(int mod, int which)
{
    size_t base = offsetof(psynth_patch_t, mod) + (size_t)mod * sizeof(psynth_mod_t);
    switch (which) {
    case 0:
        return (param_t){"SOURCE", base + offsetof(psynth_mod_t, source), 0, MOD_SOURCES - 1, false, source_names};
    case 1:
        return (param_t){"DEST", base + offsetof(psynth_mod_t, dest), 0, MOD_DESTS - 1, false, dest_names};
    default:
        return (param_t){"AMOUNT", base + offsetof(psynth_mod_t, amount), -100, 100, true, NULL};
    }
}

static int option_count(const param_t *param)
{
    return param->max - param->min + 1;
}

/* ---- Drawing ----------------------------------------------------------------------------- */

static void draw_header(void)
{
    looper_state_t state;
    looper_get_state(&state);
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    bool playing = dk_is_playing();
    put_span(&row, PLAY_SPAN, playing ? "STOP" : "PLAY", CYD_COLOR_WHITE,
             playing ? 0x0560 : CYD_COLOR_BUTTON);
    put_span(&row, REC_SPAN, "REC", CYD_COLOR_WHITE, state.recording ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    put_span(&row, BPM_DOWN_SPAN, "-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char number[8];
    snprintf(number, sizeof(number), "%u", dk_tempo());
    put_span(&row, BPM_SPAN, number, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, BPM_UP_SPAN, "+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char slot_name[4];
    snprintf(slot_name, sizeof(slot_name), "P%d", slot + 1);
    put_span(&row, SLOT_SPAN, slot_name, CYD_COLOR_BLACK, COLOR_OSC);
    put_span(&row, SWITCH_SPAN, "KEYS", CYD_COLOR_WHITE, DK_KEYS_COLOR);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_header, sizeof(row)) != 0) {
        draw_row(0, &row);
        dk_header_gaps();
        drawn_header = row;
    }
}

static void draw_page_row(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    bool listing = overlay == OVERLAY_PAGES;
    put_span(&row, TITLE_SPAN, listing ? "PAGES" : pages[page].title, CYD_COLOR_BLACK,
             listing ? CYD_COLOR_WHITE : pages[page].color);
    put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    bool presets = overlay == OVERLAY_PRESETS;
    put_span(&row, NAME_SPAN, psynth_patch(slot)->name, presets ? CYD_COLOR_BLACK : CYD_COLOR_YELLOW,
             presets ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_page_row, sizeof(row)) != 0) {
        draw_row(PAGE_ROW, &row);
        drawn_page_row = row;
    }
}

/* A slider's bar for value in min..max, from x, width wide; a signed one
   fills from its middle mark. */
static void draw_bar(int screen_row, int x, int width, int value, int min, int max, uint16_t color)
{
    int y = screen_row * FH + FH / 4;
    int height = FH / 2;
    int span = max - min;
    int position = x + (value - min) * width / span;
    int zero = min < 0 ? x + (0 - min) * width / span : x;
    ESP_ERROR_CHECK(cyd_fill_rect(x, y, width, height, COLOR_TRACK));
    int from = position < zero ? position : zero;
    int to = position < zero ? zero : position;
    if (to > from) {
        ESP_ERROR_CHECK(cyd_fill_rect(from, y, to - from, height, color));
    }
    if (min < 0) {
        ESP_ERROR_CHECK(cyd_fill_rect(zero - 1, y - 4, 2, height + 8, COLOR_ZERO));
    }
    int thumb = position - 3 < x ? x : position - 3;
    if (thumb + 6 > x + width) {
        thumb = x + width - 6;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(thumb, y - 6, 6, height + 12, CYD_COLOR_WHITE));
}

static void draw_param(int index)
{
    int screen_row = FIRST_PARAM_ROW + index;
    const page_t *p = &pages[page];
    if (index >= p->count) {
        ESP_ERROR_CHECK(cyd_fill_rect(0, screen_row * FH, CYD_LCD_WIDTH, FH, CYD_COLOR_BLACK));
        return;
    }
    const param_t *param = &p->params[index];
    int value = get_value(param);
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    if (param->choices != NULL) {
        memcpy(row.text, param->label, strnlen(param->label, LABEL_COLUMNS));
        put_span(&row, CHOICE_SPAN, param->choices[value], CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
        draw_row(screen_row, &row);
        return;
    }
    char label[LABEL_COLUMNS + 8];
    snprintf(label, sizeof(label), "%-5.5s%4d", param->label, value);
    memcpy(row.text, label, strnlen(label, LABEL_COLUMNS));
    draw_row(screen_row, &row);
    draw_bar(screen_row, BAR_X, BAR_WIDTH, value, param->min, param->max, p->color);
}

/* A matrix row: SOURCE and DEST buttons, the amount and its bar. */
static void draw_matrix_row(int index)
{
    int screen_row = FIRST_PARAM_ROW + index;
    const page_t *p = &pages[page];
    if (index >= p->count) {
        ESP_ERROR_CHECK(cyd_fill_rect(0, screen_row * FH, CYD_LCD_WIDTH, FH, CYD_COLOR_BLACK));
        return;
    }
    const psynth_mod_t *mod = &psynth_patch(slot)->mod[p->first + index];
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    bool on = mod->source != SOURCE_NONE && mod->dest != DEST_NONE;
    put_span(&row, SOURCE_SPAN, source_names[mod->source % MOD_SOURCES],
             mod->source != SOURCE_NONE ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             mod->source != SOURCE_NONE ? COLOR_MOD : CYD_COLOR_BUTTON);
    put_span(&row, DEST_SPAN, dest_names[mod->dest % MOD_DESTS],
             mod->dest != DEST_NONE ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             mod->dest != DEST_NONE ? COLOR_MOD : CYD_COLOR_BUTTON);
    char amount[8];
    snprintf(amount, sizeof(amount), "%+4d", mod->amount);
    memcpy(row.text + MATRIX_VALUE_COLUMN, amount, 4);
    draw_row(screen_row, &row);
    draw_dividers(screen_row, (const span_t[]){SOURCE_SPAN, DEST_SPAN}, 2);
    draw_bar(screen_row, MATRIX_BAR_X, MATRIX_BAR_WIDTH, mod->amount, -100, 100, on ? COLOR_MOD : COLOR_ZERO);
}

/* What a settings row shows, as a number, so it redraws on a change. */
static int row_value(int index)
{
    const page_t *p = &pages[page];
    if (index >= p->count) {
        return -9999;
    }
    if (p->kind == PAGE_MATRIX) {
        const psynth_mod_t *mod = &psynth_patch(slot)->mod[p->first + index];
        return (mod->source << 16) | (mod->dest << 8) | (uint8_t)mod->amount;
    }
    return get_value(&p->params[index]);
}

static void draw_scope_row_buttons(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, OCT_DOWN_SPAN, "OC-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, OCT_UP_SPAN, "OC+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(SCOPE_ROW, &row);
}

/* The oscilloscope: the edited patch's last few ms, from a rising zero
   crossing so a steady note stands still. */
static void draw_scope(void)
{
    static float samples[1024];
    int count = psynth_scope(samples, 1024);
    int start = 0;
    for (int i = 1; i < count - SCOPE_WIDTH; ++i) {
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f) {
            start = i;
            break;
        }
    }
    int top = SCOPE_ROW * FH + 2;
    int height = FH - 4;
    int middle = top + height / 2;
    ESP_ERROR_CHECK(cyd_fill_rect(SCOPE_X, top, SCOPE_WIDTH, height, COLOR_SCOPE_BACK));
    ESP_ERROR_CHECK(cyd_fill_rect(SCOPE_X, middle, SCOPE_WIDTH, 1, 0x2945));
    int previous = middle;
    for (int x = 0; x < SCOPE_WIDTH && start + x < count; ++x) {
        float s = samples[start + x] * 1.6f;
        s = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
        int y = middle - (int)(s * (height / 2 - 1));
        int y0 = y < previous ? y : previous;
        int y1 = y < previous ? previous : y;
        ESP_ERROR_CHECK(cyd_fill_rect(SCOPE_X + x, y0, 1, y1 - y0 + 1, COLOR_SCOPE));
        previous = y;
    }
}

static void draw_test_keys(void)
{
    static const char *const names[] = {"C", "D", "E", "F", "G", "A", "B", "C"};
    static const int steps[TEST_KEYS] = {0, 2, 4, 5, 7, 9, 11, 12};
    ESP_ERROR_CHECK(cyd_fill_rect(0, KEYS_TOP, CYD_LCD_WIDTH, CYD_LCD_HEIGHT - KEYS_TOP, CYD_COLOR_BLACK));
    for (int key = 0; key < TEST_KEYS; ++key) {
        int x = key * KEY_WIDTH + 2;
        ESP_ERROR_CHECK(cyd_fill_rect(x, KEYS_TOP + 2, KEY_WIDTH - 4, CYD_LCD_HEIGHT - KEYS_TOP - 4,
                                      CYD_COLOR_WHITE));
        char name[8];
        snprintf(name, sizeof(name), "%s%d", names[key], (base_note + steps[key]) / 12 - 1);
        ESP_ERROR_CHECK(cyd_draw_small_text(x + 8, CYD_LCD_HEIGHT - CYD_SMALL_FONT_HEIGHT - 10, name,
                                            CYD_COLOR_BLACK, CYD_COLOR_WHITE));
    }
}

static void draw_text_row(int screen_row, const char *text, uint16_t fg)
{
    row_t row;
    clear_row(&row, fg);
    memcpy(row.text, text, strnlen(text, COLUMNS));
    draw_row(screen_row, &row);
}

static const span_t RACK_LAYER_SPAN = {LABEL_COLUMNS, COLUMNS - LABEL_COLUMNS};
static const span_t RACK_TOGGLE_SPAN = {0, 4};
static const span_t SAMPLE_SAVE_SPAN = {LABEL_COLUMNS, 8};
static const span_t SAMPLE_PLAY_SPAN = {LABEL_COLUMNS + 9, COLUMNS - LABEL_COLUMNS - 9};

static uint32_t hash_bytes(uint32_t sig, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        sig = (sig ^ bytes[i]) * 16777619u;
    }
    return sig;
}

/* A number standing for what a custom page or an overlay shows. */
static uint32_t custom_signature(void)
{
    uint32_t sig = 2166136261u;
    int words[] = {overlay, page, slot, category, osc_only, undo_slot, chosen_sample, psynth_slot_preset(slot)};
    sig = hash_bytes(sig, words, sizeof(words));
    if (overlay == OVERLAY_CHOICE) {
        int value = get_value(&choice);
        sig = hash_bytes(sig, &value, sizeof(value));
    }
    if (overlay == OVERLAY_PRESETS) {
        sig = hash_bytes(sig, psynth_patch(slot)->name, PSYNTH_NAME_MAX);
    }
    sig = hash_bytes(sig, psynth_rack(), sizeof(psynth_rack_t));
    for (int i = 0; i < PSYNTH_SAMPLES; ++i) {
        uint32_t tenths = (uint32_t)(psynth_sample_seconds(i) * 10.0f);
        sig = hash_bytes(sig, &tenths, sizeof(tenths));
    }
    return hash_bytes(sig, sample_status, strlen(sample_status));
}

static void draw_rack(void)
{
    psynth_rack_t *rack = psynth_rack();
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    memcpy(row.text, "LAYER", 5);
    put_span(&row, RACK_LAYER_SPAN, rack->layer ? "ON: KEYS PLAY ALL ON" : "OFF: ONE PATCH AT A TIME",
             rack->layer ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             rack->layer ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    draw_row(FIRST_PARAM_ROW, &row);
    for (int i = 0; i < PSYNTH_SLOTS; ++i) {
        int screen_row = FIRST_PARAM_ROW + 1 + i;
        clear_row(&row, CYD_COLOR_WHITE);
        char name[8];
        snprintf(name, sizeof(name), "P%d", i + 1);
        put_span(&row, RACK_TOGGLE_SPAN, name, rack->on[i] ? CYD_COLOR_BLACK : CYD_COLOR_GRAY,
                 rack->on[i] ? COLOR_OSC : CYD_COLOR_BUTTON_DISABLED);
        char level[8];
        snprintf(level, sizeof(level), "%4d", rack->level[i]);
        memcpy(row.text + 4, level, 4);
        draw_row(screen_row, &row);
        draw_bar(screen_row, BAR_X, BAR_WIDTH, rack->level[i], 0, 100, rack->on[i] ? COLOR_OSC : COLOR_ZERO);
    }
    char hint[COLUMNS + 1];
    snprintf(hint, sizeof(hint), " P1-P4 ON: in the layer. %s", psynth_patch(slot)->name);
    draw_text_row(FIRST_PARAM_ROW + 5, hint, CYD_COLOR_GRAY);
}

static void draw_samples(void)
{
    for (int i = 0; i < PSYNTH_SAMPLES; ++i) {
        int screen_row = FIRST_PARAM_ROW + i;
        row_t row;
        clear_row(&row, CYD_COLOR_WHITE);
        char label[16];
        float seconds = psynth_sample_seconds(i);
        if (seconds > 0.0f) {
            snprintf(label, sizeof(label), "S%d %4.1fs", i + 1, (double)seconds);
        } else {
            snprintf(label, sizeof(label), "S%d EMPTY", i + 1);
        }
        memcpy(row.text, label, strnlen(label, LABEL_COLUMNS));
        char save[12];
        snprintf(save, sizeof(save), "SAVE P%d", slot + 1);
        put_span(&row, SAMPLE_SAVE_SPAN, save, CYD_COLOR_WHITE, CYD_COLOR_RED);
        bool chosen = chosen_sample == i;
        put_span(&row, SAMPLE_PLAY_SPAN, seconds > 0.0f ? (chosen ? "ON TEST KEYS" : "PLAY ON KEYS") : "-",
                 chosen ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                 seconds > 0.0f ? (chosen ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON) : CYD_COLOR_BUTTON_DISABLED);
        draw_row(screen_row, &row);
    }
    draw_text_row(FIRST_PARAM_ROW + 4, sample_status, CYD_COLOR_YELLOW);
    draw_text_row(FIRST_PARAM_ROW + 5, " Samples play as PATCH styles S1-S4; ROLL: INST", CYD_COLOR_GRAY);
}

/* Every page, three to a row in its group's color, and PRESETS. */
static void draw_page_list(void)
{
    for (int r = 0; r < PARAM_ROWS; ++r) {
        row_t row;
        clear_row(&row, CYD_COLOR_WHITE);
        for (int c = 0; c < LIST_COLUMNS; ++c) {
            int i = r * LIST_COLUMNS + c;
            if (i < PAGES) {
                bool here = i == page;
                put_span(&row, LIST_SPANS[c], pages[i].title, here ? CYD_COLOR_WHITE : CYD_COLOR_BLACK,
                         here ? CYD_COLOR_BLACK : pages[i].color);
            } else if (i == PAGES) {
                put_span(&row, LIST_SPANS[c], "PRESETS", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
            }
        }
        draw_row(FIRST_PARAM_ROW + r, &row);
        draw_dividers(FIRST_PARAM_ROW + r, LIST_SPANS, LIST_COLUMNS);
    }
}

/* The presets in the chosen category, up to PRESETS_SHOWN. */
static int category_presets(int *out)
{
    int count = 0;
    for (int i = 0; i < psynth_preset_count() && count < PRESETS_SHOWN; ++i) {
        if (psynth_preset_category(i) == category) {
            out[count++] = i;
        }
    }
    return count;
}

static void draw_presets(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    for (int c = 0; c < CATEGORIES; ++c) {
        bool here = c == category;
        put_span(&row, CATEGORY_SPANS[c], psynth_category_name(c), here ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                 here ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON);
    }
    draw_row(FIRST_PARAM_ROW, &row);
    draw_dividers(FIRST_PARAM_ROW, CATEGORY_SPANS, CATEGORIES);

    int list[PRESETS_SHOWN];
    int count = category_presets(list);
    int current = psynth_slot_preset(slot);
    for (int r = 0; r < PRESET_ROWS; ++r) {
        clear_row(&row, CYD_COLOR_WHITE);
        for (int c = 0; c < 2; ++c) {
            int i = c * PRESET_ROWS + r;
            if (i < count) {
                bool here = list[i] == current;
                put_span(&row, HALF_SPANS[c], psynth_preset_name(list[i]), here ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                         here ? COLOR_OSC : CYD_COLOR_BUTTON);
            }
        }
        draw_row(FIRST_PARAM_ROW + 1 + r, &row);
        draw_dividers(FIRST_PARAM_ROW + 1 + r, HALF_SPANS, 2);
    }

    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, LOAD_SPAN, osc_only ? "OSC ONLY" : "LOAD ALL", CYD_COLOR_BLACK,
             osc_only ? COLOR_FILTER : CYD_COLOR_CYAN);
    bool can_undo = undo_slot == slot;
    put_span(&row, UNDO_SPAN, "UNDO", can_undo ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_undo ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, INIT_SPAN, "INIT", CYD_COLOR_WHITE, CYD_COLOR_RED);
    put_span(&row, DONE_SPAN, "DONE", CYD_COLOR_WHITE, 0x0560);
    draw_row(FIRST_PARAM_ROW + 5, &row);
    const span_t tools[] = {LOAD_SPAN, UNDO_SPAN, INIT_SPAN, DONE_SPAN};
    draw_dividers(FIRST_PARAM_ROW + 5, tools, 4);
}

static void draw_choice(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_CYAN);
    memcpy(row.text, choice_title, strnlen(choice_title, BACK_SPAN.column));
    put_span(&row, BACK_SPAN, "BACK", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(FIRST_PARAM_ROW, &row);
    int value = get_value(&choice);
    int count = option_count(&choice);
    for (int r = 0; r < CHOICE_ROWS; ++r) {
        clear_row(&row, CYD_COLOR_WHITE);
        for (int c = 0; c < 2; ++c) {
            int i = c * CHOICE_ROWS + r;
            if (i < count) {
                bool here = choice.min + i == value;
                put_span(&row, HALF_SPANS[c], choice.choices[i], here ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                         here ? pages[page].color : CYD_COLOR_BUTTON);
            }
        }
        draw_row(FIRST_PARAM_ROW + 1 + r, &row);
        draw_dividers(FIRST_PARAM_ROW + 1 + r, HALF_SPANS, 2);
    }
}

/* ---- Touch ------------------------------------------------------------------------------- */

static int test_note_at(int x, int y)
{
    static const int steps[TEST_KEYS] = {0, 2, 4, 5, 7, 9, 11, 12};
    if (y < KEYS_TOP) {
        return -1;
    }
    int key = x / KEY_WIDTH;
    key = key < TEST_KEYS ? key : TEST_KEYS - 1;
    return base_note + steps[key];
}

/* The touch task's fingers callback: test keys play while held. */
static void on_fingers(const cyd_touch_fingers_t *report, void *context)
{
    (void)context;
    bool seen[MAX_FINGERS] = {false};
    int instrument = test_instrument;
    for (int i = 0; i < report->count; ++i) {
        int id = report->ids[i] % MAX_FINGERS;
        int note = test_note_at(report->points[i].x, report->points[i].y);
        seen[id] = true;
        if (note != finger_notes[id]) {
            if (finger_notes[id] >= 0) {
                keys_note_off(finger_notes[id], instrument);
            }
            if (note >= 0) {
                keys_note_on(note, instrument);
            }
            finger_notes[id] = (int16_t)note;
        }
    }
    for (int id = 0; id < MAX_FINGERS; ++id) {
        if (!seen[id] && finger_notes[id] >= 0) {
            keys_note_off(finger_notes[id], instrument);
            finger_notes[id] = -1;
        }
    }
}

/* Taps on the test keys are notes, handled by on_fingers. */
static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    return point->y >= KEYS_TOP;
}

static void update_test_instrument(void)
{
    bool sample = pages[page].kind == PAGE_SAMPLES && chosen_sample >= 0;
    test_instrument = synth_instrument(VOICE_PATCH, sample ? PSYNTH_SLOTS + chosen_sample : slot);
}

static void lift_test_keys(void)
{
    keys_all_notes_off();
    memset(finger_notes, 0xFF, sizeof(finger_notes));
}

static void set_slot(int new_slot)
{
    lift_test_keys();
    slot = new_slot;
    update_test_instrument();
    psynth_set_scope_slot(slot);
}

static void set_page(int new_page)
{
    page = (new_page + PAGES) % PAGES;
    overlay = OVERLAY_NONE;
    update_test_instrument();
}

/* Shows rows 2-7 as an overlay, or the page again with OVERLAY_NONE. */
static void set_overlay(overlay_t new_overlay)
{
    overlay = new_overlay;
    drawn_page = -1; /* rows 2-7 are drawn from scratch */
}

static void open_choice(const param_t *param, const char *title)
{
    choice = *param;
    snprintf(choice_title, sizeof(choice_title), " %s", title);
    set_overlay(OVERLAY_CHOICE);
}

/* A choice: up to three options step on a tap; more open a list. */
static void tap_choice(const param_t *param)
{
    if (option_count(param) <= 3) {
        int value = get_value(param) + 1;
        set_value(param, value > param->max ? param->min : value);
    } else {
        char title[COLUMNS + 1];
        snprintf(title, sizeof(title), "%s %s", pages[page].title, param->label);
        open_choice(param, title);
    }
}

static void tap_header(int column, const looper_state_t *state)
{
    if (in_span(PLAY_SPAN, column)) {
        dk_play(!dk_is_playing());
    } else if (in_span(REC_SPAN, column)) {
        if (!state->recording && !dk_is_playing()) {
            dk_play(true);
        }
        keys_record(!state->recording);
    } else if (in_span(BPM_DOWN_SPAN, column)) {
        dk_set_tempo(dk_tempo() - 1);
    } else if (in_span(BPM_UP_SPAN, column)) {
        dk_set_tempo(dk_tempo() + 1);
    } else if (in_span(SLOT_SPAN, column)) {
        set_slot((slot + 1) % PSYNTH_SLOTS);
    }
}

static void tap_page_row(int column)
{
    if (in_span(PREV_SPAN, column)) {
        set_page(page - 1);
    } else if (in_span(NEXT_SPAN, column)) {
        set_page(page + 1);
    } else if (in_span(TITLE_SPAN, column)) {
        set_overlay(overlay == OVERLAY_PAGES ? OVERLAY_NONE : OVERLAY_PAGES);
    } else if (in_span(NAME_SPAN, column)) {
        if (overlay != OVERLAY_PRESETS) {
            int current = psynth_preset_category(psynth_slot_preset(slot));
            category = current >= 0 ? current : category;
        }
        set_overlay(overlay == OVERLAY_PRESETS ? OVERLAY_NONE : OVERLAY_PRESETS);
    }
}

static void tap_rack(int index, int x)
{
    psynth_rack_t *rack = psynth_rack();
    if (index == 0 && x >= LABEL_COLUMNS * FW) {
        rack->layer = !rack->layer;
        keys_all_notes_off(); /* notes held now belong to the old setting */
    } else if (index >= 1 && index <= PSYNTH_SLOTS && x < 4 * FW) {
        rack->on[index - 1] = !rack->on[index - 1];
    }
}

static void tap_samples(int index, int x)
{
    if (index >= PSYNTH_SAMPLES) {
        return;
    }
    int column = x / FW;
    if (in_span(SAMPLE_SAVE_SPAN, column)) {
        snprintf(sample_status, sizeof(sample_status), " Rendering %s into S%d...",
                 psynth_patch(slot)->name, index + 1);
        draw_text_row(FIRST_PARAM_ROW + 4, sample_status, CYD_COLOR_YELLOW);
        bool ok = psynth_render_sample(slot, index);
        snprintf(sample_status, sizeof(sample_status), ok ? " Saved S%d: %.1f s of %s" : " No memory for S%d",
                 index + 1, (double)psynth_sample_seconds(index), psynth_patch(slot)->name);
        if (ok) {
            chosen_sample = index;
        }
    } else if (in_span(SAMPLE_PLAY_SPAN, column) && psynth_sample_seconds(index) > 0.0f) {
        chosen_sample = chosen_sample == index ? -1 : index;
    }
    lift_test_keys();
    update_test_instrument();
}

static void tap_matrix(int index, int column)
{
    const page_t *p = &pages[page];
    if (index >= p->count) {
        return;
    }
    int mod = p->first + index;
    char title[16];
    if (in_span(SOURCE_SPAN, column)) {
        param_t param = matrix_param(mod, 0);
        snprintf(title, sizeof(title), "MOD %d SOURCE", mod + 1);
        open_choice(&param, title);
    } else if (in_span(DEST_SPAN, column)) {
        param_t param = matrix_param(mod, 1);
        snprintf(title, sizeof(title), "MOD %d DEST", mod + 1);
        open_choice(&param, title);
    }
}

static void tap_param(int index, int x)
{
    const page_t *p = &pages[page];
    switch (p->kind) {
    case PAGE_RACK:
        tap_rack(index, x);
        return;
    case PAGE_SAMPLES:
        tap_samples(index, x);
        return;
    case PAGE_MATRIX:
        tap_matrix(index, x / FW);
        return;
    default:
        break;
    }
    if (index >= p->count) {
        return;
    }
    const param_t *param = &p->params[index];
    if (param->choices != NULL && x >= LABEL_COLUMNS * FW) {
        tap_choice(param);
    }
}

static void tap_page_list(int index, int column)
{
    for (int c = 0; c < LIST_COLUMNS; ++c) {
        int i = index * LIST_COLUMNS + c;
        if (!in_span(LIST_SPANS[c], column)) {
            continue;
        }
        if (i < PAGES) {
            set_page(i);
            set_overlay(OVERLAY_NONE);
        } else if (i == PAGES) {
            set_overlay(OVERLAY_PRESETS);
        }
    }
}

/* Loads a preset into the patch, keeping what was there for UNDO. */
static void load(int preset)
{
    undo_patch = *psynth_patch(slot);
    undo_slot = slot;
    psynth_load(slot, preset, osc_only);
}

static void tap_presets(int index, int column)
{
    if (index == 0) {
        for (int c = 0; c < CATEGORIES; ++c) {
            if (in_span(CATEGORY_SPANS[c], column)) {
                category = c;
            }
        }
    } else if (index <= PRESET_ROWS) {
        int list[PRESETS_SHOWN];
        int count = category_presets(list);
        int i = (column >= HALF_SPANS[1].column ? PRESET_ROWS : 0) + index - 1;
        if (i < count) {
            load(list[i]);
        }
    } else if (in_span(LOAD_SPAN, column)) {
        osc_only = !osc_only;
    } else if (in_span(UNDO_SPAN, column) && undo_slot == slot) {
        *psynth_patch(slot) = undo_patch;
        undo_slot = -1;
    } else if (in_span(INIT_SPAN, column)) {
        load(psynth_preset_init());
    } else if (in_span(DONE_SPAN, column)) {
        set_overlay(OVERLAY_NONE);
    }
}

static void tap_choice_list(int index, int column)
{
    if (index == 0) {
        if (in_span(BACK_SPAN, column)) {
            set_overlay(OVERLAY_NONE);
        }
        return;
    }
    int i = (column >= HALF_SPANS[1].column ? CHOICE_ROWS : 0) + index - 1;
    if (i < option_count(&choice)) {
        set_value(&choice, choice.min + i);
        set_overlay(OVERLAY_NONE);
    }
}

/* Fingers held on the sliders set them, so a drag sweeps a value. */
static void follow_sliders(void)
{
    dragging = false;
    if (overlay != OVERLAY_NONE) {
        return;
    }
    cyd_touch_point_t held[5];
    int count = cyd_touch_held(held, 5);
    const page_t *p = &pages[page];
    for (int i = 0; i < count; ++i) {
        int index = held[i].y / FH - FIRST_PARAM_ROW;
        if (p->kind == PAGE_RACK) {
            if (index >= 1 && index <= PSYNTH_SLOTS && held[i].x >= BAR_X - FW) {
                int value = ((held[i].x - BAR_X) * 100 + BAR_WIDTH / 2) / BAR_WIDTH;
                psynth_rack()->level[index - 1] = (uint8_t)(value < 0 ? 0 : value > 100 ? 100 : value);
                dragging = true;
            }
            continue;
        }
        if (index < 0 || index >= p->count || p->kind == PAGE_SAMPLES) {
            continue;
        }
        param_t param;
        int x = BAR_X, width = BAR_WIDTH;
        if (p->kind == PAGE_MATRIX) {
            if (held[i].x < MATRIX_BAR_X - FW) {
                continue;
            }
            param = matrix_param(p->first + index, 2);
            x = MATRIX_BAR_X;
            width = MATRIX_BAR_WIDTH;
        } else {
            if (held[i].x < BAR_X - FW) {
                continue;
            }
            param = p->params[index];
            if (param.choices != NULL) {
                continue;
            }
        }
        dragging = true;
        int span = param.max - param.min;
        int value = param.min + ((held[i].x - x) * span + width / 2) / width;
        if (param.min < 0 && value != 0 && abs(value) <= span / 24) {
            value = 0; /* snaps to the middle */
        }
        set_value(&param, value);
    }
}

/* ---- The screen ----------------------------------------------------------------------------- */

static void draw_area(void)
{
    bool all = page != drawn_page || slot != drawn_slot;
    bool custom = overlay != OVERLAY_NONE || pages[page].kind == PAGE_RACK || pages[page].kind == PAGE_SAMPLES;
    if (custom) {
        uint32_t sig = custom_signature();
        if (all || sig != drawn_custom) {
            if (all) {
                ESP_ERROR_CHECK(cyd_fill_rect(0, FIRST_PARAM_ROW * FH, CYD_LCD_WIDTH, PARAM_ROWS * FH,
                                              CYD_COLOR_BLACK));
            }
            switch (overlay) {
            case OVERLAY_PAGES:
                draw_page_list();
                break;
            case OVERLAY_PRESETS:
                draw_presets();
                break;
            case OVERLAY_CHOICE:
                draw_choice();
                break;
            default:
                if (pages[page].kind == PAGE_RACK) {
                    draw_rack();
                } else {
                    draw_samples();
                }
                break;
            }
            drawn_custom = sig;
        }
    } else {
        for (int index = 0; index < PARAM_ROWS; ++index) {
            int value = row_value(index);
            if (all || value != drawn_values[index]) {
                if (pages[page].kind == PAGE_MATRIX) {
                    draw_matrix_row(index);
                } else {
                    draw_param(index);
                }
                drawn_values[index] = value;
            }
        }
    }
    drawn_page = page;
    drawn_slot = slot;
}

static void leave(void)
{
    cyd_touch_set_fingers_callback(NULL, NULL);
    cyd_touch_set_tap_callback(NULL, NULL);
    cyd_touch_set_fast(false);
    keys_all_notes_off();
}

static void enter(QueueHandle_t touch_queue)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    memset(&drawn_header, 0, sizeof(drawn_header));
    memset(&drawn_page_row, 0, sizeof(drawn_page_row));
    drawn_page = -1;
    drawn_slot = -1;
    drawn_base = -1;
    memset(finger_notes, 0xFF, sizeof(finger_notes));
    update_test_instrument();
    psynth_set_scope_slot(slot);
    draw_scope_row_buttons();
    xQueueReset(touch_queue);
    cyd_touch_set_tap_callback(on_tap, NULL);
    cyd_touch_set_fingers_callback(on_fingers, NULL);
    cyd_touch_set_fast(true);
}

/* Runs the SYNTH screen until KEYS is tapped. */
void synth_screen(QueueHandle_t touch_queue)
{
    enter(touch_queue);
    while (true) {
        looper_state_t state;
        looper_get_state(&state);
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        while (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            int row = point.y / FH;
            int column = point.x / FW;
            if (point.y >= KEYS_TOP) {
                continue; /* came before the keys went live */
            }
            if (row == 0) {
                if (in_span(SWITCH_SPAN, column)) {
                    leave();
                    return; /* on to the keys */
                }
                if (in_span(MENU_SPAN, column)) {
                    leave();
                    keys_menu(touch_queue);
                    enter(touch_queue);
                    break;
                }
                tap_header(column, &state);
            } else if (row == PAGE_ROW) {
                tap_page_row(column);
            } else if (row == SCOPE_ROW) {
                if (in_span(OCT_DOWN_SPAN, column) && base_note > 24) {
                    base_note -= 12;
                } else if (in_span(OCT_UP_SPAN, column) && base_note < 84) {
                    base_note += 12;
                }
            } else if (row >= FIRST_PARAM_ROW && row < FIRST_PARAM_ROW + PARAM_ROWS) {
                int index = row - FIRST_PARAM_ROW;
                switch (overlay) {
                case OVERLAY_PAGES:
                    tap_page_list(index, column);
                    break;
                case OVERLAY_PRESETS:
                    tap_presets(index, column);
                    break;
                case OVERLAY_CHOICE:
                    tap_choice_list(index, column);
                    break;
                default:
                    tap_param(index, point.x);
                    break;
                }
            }
        }
        follow_sliders();

        draw_header();
        draw_page_row();
        draw_area();
        if (base_note != drawn_base) {
            draw_test_keys();
            drawn_base = base_note;
        }
        TickType_t now = xTaskGetTickCount();
        if (!dragging && now - scope_at >= pdMS_TO_TICKS(SCOPE_MS)) {
            draw_scope();
            scope_at = now;
        }
        /* The keys screen lights keys from these; drop them while away. */
        int heard_note;
        bool heard_on;
        while (keys_next_heard_note(&heard_note, &heard_on)) {
        }
        if (wait == 0) {
            vTaskDelay(1);
        }
    }
}
