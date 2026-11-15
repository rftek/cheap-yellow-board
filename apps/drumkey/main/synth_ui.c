/* Drum&Key's SYNTH screen: edits the patch synth's patches (psynth.h),
   which the keys and the Piano Roll play as the PATCH voice's styles.

     row 0       PLAY REC - 120 + P1 KEYS MENU   (P1-P4 picks the patch)
     row 1       <  OSC 1  >   SUPERSAW   INIT    page, patch name, preset
     rows 2-7    up to six settings: sliders (drag) or choices (tap)
     row 8       OCT-  [ oscilloscope ]  OCT+
     rows 9-11   eight test keys, C to C, playing the patch

   Pages: RACK, OSC 1, OSC 2, SUB+NOISE, FILTER 1, FILTER 2, AMP ENV,
   MOD ENV, LFOS, MOD 1-2, MOD 3-4, MOD 5-6, MOD 7-8, FX 1, FX 2, SAMPLES.
   RACK sets LAYER (a key plays every patch switched on) and each patch's
   level; SAMPLES renders the patch into a sample slot.

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

#define COLOR_TRACK 0x2104
#define COLOR_ZERO 0x7BEF
#define COLOR_SCOPE_BACK 0x0841
#define COLOR_SCOPE 0x07E0

static const span_t PLAY_SPAN = DK_PLAY_SPAN;
static const span_t REC_SPAN = DK_REC_SPAN;
static const span_t BPM_DOWN_SPAN = DK_BPM_DOWN_SPAN;
static const span_t BPM_SPAN = DK_BPM_SPAN;
static const span_t BPM_UP_SPAN = DK_BPM_UP_SPAN;
static const span_t SLOT_SPAN = DK_SCREEN_SPAN;
static const span_t SWITCH_SPAN = DK_SWITCH_SPAN;
static const span_t MENU_SPAN = DK_MENU_SPAN;

static const span_t PREV_SPAN = {0, 2};
static const span_t TITLE_SPAN = {2, 10};
static const span_t NEXT_SPAN = {12, 2};
static const span_t NAME_SPAN = {14, 8};
static const span_t INIT_SPAN = {22, 4};
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

typedef enum { PAGE_PARAMS, PAGE_RACK, PAGE_SAMPLES } page_kind_t;

typedef struct {
    const char *title;
    int count;
    param_t params[PARAM_ROWS];
    page_kind_t kind;
} page_t;

#define OSC_PAGE(title, i)                                                                 \
    {title, 6,                                                                             \
     {CHOICE("SHAPE", osc[i].shape, shape_names), SLIDER("POS", osc[i].position, 0, 100), \
      SLIDER("UNISON", osc[i].unison, 1, PSYNTH_UNISON_MAX),                               \
      SLIDER("DETUNE", osc[i].detune, 0, 100), SIGNED("SEMI", osc[i].semi, -24, 24),     \
      SLIDER("LEVEL", osc[i].level, 0, 100)}, PAGE_PARAMS}
#define ENV_PAGE(title, i)                                                                   \
    {title, 4,                                                                               \
     {SLIDER("ATTACK", env[i].attack, 0, 100), SLIDER("DECAY", env[i].decay, 0, 100),     \
      SLIDER("SUSTAIN", env[i].sustain, 0, 100), SLIDER("RELEASE", env[i].release, 0, 100)}, PAGE_PARAMS}
#define MOD_PAGE(title, a, b)                                                               \
    {title, 6,                                                                              \
     {CHOICE("SRC", mod[a].source, source_names), CHOICE("DEST", mod[a].dest, dest_names), \
      SIGNED("AMOUNT", mod[a].amount, -100, 100), CHOICE("SRC", mod[b].source, source_names), \
      CHOICE("DEST", mod[b].dest, dest_names), SIGNED("AMOUNT", mod[b].amount, -100, 100)}, PAGE_PARAMS}

static const page_t pages[] = {
    {"RACK", 0, {{0}}, PAGE_RACK},
    OSC_PAGE("OSC 1", 0),
    OSC_PAGE("OSC 2", 1),
    {"SUB+NOISE", 5,
     {SLIDER("SUB", sub_level, 0, 100), CHOICE("SUB WAVE", sub_shape, sub_names),
      SLIDER("NOISE", noise_level, 0, 100), SLIDER("COLOR", noise_color, 0, 100),
      SLIDER("VOLUME", volume, 0, 100)}, PAGE_PARAMS},
    {"FILTER 1", 6,
     {CHOICE("TYPE", filter[0].type, filter_names), SLIDER("CUTOFF", filter[0].cutoff, 0, 100),
      SLIDER("RESO", filter[0].resonance, 0, 100), SLIDER("DRIVE", filter[0].drive, 0, 100),
      SLIDER("KEYTRK", keytrack, 0, 100), CHOICE("ROUTING", routing, routing_names)}, PAGE_PARAMS},
    {"FILTER 2", 4,
     {CHOICE("TYPE", filter[1].type, filter_names), SLIDER("CUTOFF", filter[1].cutoff, 0, 100),
      SLIDER("RESO", filter[1].resonance, 0, 100), SLIDER("DRIVE", filter[1].drive, 0, 100)}, PAGE_PARAMS},
    ENV_PAGE("AMP ENV", 0),
    ENV_PAGE("MOD ENV", 1),
    {"LFOS", 6,
     {CHOICE("LFO1", lfo[0].shape, lfo_names), SLIDER("L1 RATE", lfo[0].rate, 0, 100),
      CHOICE("L1 SYNC", lfo[0].sync, sync_names), CHOICE("LFO2", lfo[1].shape, lfo_names),
      SLIDER("L2 RATE", lfo[1].rate, 0, 100), CHOICE("L2 SYNC", lfo[1].sync, sync_names)}, PAGE_PARAMS},
    MOD_PAGE("MOD 1-2", 0, 1),
    MOD_PAGE("MOD 3-4", 2, 3),
    MOD_PAGE("MOD 5-6", 4, 5),
    MOD_PAGE("MOD 7-8", 6, 7),
    {"FX 1", 5,
     {SLIDER("DRIVE", drive, 0, 100), SLIDER("CHORUS", chorus, 0, 100),
      CHOICE("DLY TIME", delay_time, delay_names), SLIDER("DLY FDBK", delay_feedback, 0, 100),
      SLIDER("DLY MIX", delay_mix, 0, 100)}, PAGE_PARAMS},
    {"FX 2", 3,
     {SLIDER("REV SIZE", reverb_size, 0, 100), SLIDER("REV MIX", reverb_mix, 0, 100),
      SLIDER("VOLUME", volume, 0, 100)}, PAGE_PARAMS},
    {"SAMPLES", 0, {{0}}, PAGE_SAMPLES},
};
#define PAGES ((int)(sizeof(pages) / sizeof(pages[0])))

/* ---- State ---------------------------------------------------------------------------- */

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

static int slot;
static int page;
static int base_note = 48; /* C3 */
static bool init_armed;
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
/* What the custom pages showed, so they redraw only on a change. */
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

static uint16_t patch_color(void)
{
    return 0xFAD2; /* the PATCH voice's pink */
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
    put_span(&row, SLOT_SPAN, slot_name, CYD_COLOR_BLACK, patch_color());
    put_span(&row, SWITCH_SPAN, "KEYS", CYD_COLOR_WHITE, DK_KEYS_COLOR);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_header, sizeof(row)) != 0) {
        ESP_ERROR_CHECK(cyd_draw_cells(0, row.text, row.fg, row.bg));
        dk_header_gaps();
        drawn_header = row;
    }
}

static void draw_page_row(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, TITLE_SPAN, pages[page].title, CYD_COLOR_BLACK, CYD_COLOR_CYAN);
    put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, NAME_SPAN, psynth_patch(slot)->name, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, INIT_SPAN, init_armed ? "INIT?" : "INIT", CYD_COLOR_WHITE,
             init_armed ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_page_row, sizeof(row)) != 0) {
        ESP_ERROR_CHECK(cyd_draw_cells(PAGE_ROW, row.text, row.fg, row.bg));
        drawn_page_row = row;
    }
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
        ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row.text, row.fg, row.bg));
        return;
    }
    char label[LABEL_COLUMNS + 8];
    snprintf(label, sizeof(label), "%-5.5s%4d", param->label, value);
    memcpy(row.text, label, strnlen(label, LABEL_COLUMNS));
    ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row.text, row.fg, row.bg));
    int y = screen_row * FH + FH / 4;
    int height = FH / 2;
    int span = param->max - param->min;
    int position = BAR_X + (value - param->min) * BAR_WIDTH / span;
    int zero = param->min < 0 ? BAR_X + (0 - param->min) * BAR_WIDTH / span : BAR_X;
    ESP_ERROR_CHECK(cyd_fill_rect(BAR_X, y, BAR_WIDTH, height, COLOR_TRACK));
    int from = position < zero ? position : zero;
    int to = position < zero ? zero : position;
    if (to > from) {
        ESP_ERROR_CHECK(cyd_fill_rect(from, y, to - from, height, patch_color()));
    }
    if (param->min < 0) {
        ESP_ERROR_CHECK(cyd_fill_rect(zero - 1, y - 4, 2, height + 8, COLOR_ZERO));
    }
    int thumb = position - 3 < BAR_X ? BAR_X : position - 3;
    if (thumb + 6 > BAR_X + BAR_WIDTH) {
        thumb = BAR_X + BAR_WIDTH - 6;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(thumb, y - 6, 6, height + 12, CYD_COLOR_WHITE));
}

static void draw_scope_row_buttons(void)
{
    row_t row;
    clear_row(&row, CYD_COLOR_WHITE);
    put_span(&row, OCT_DOWN_SPAN, "OC-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, OCT_UP_SPAN, "OC+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(SCOPE_ROW, row.text, row.fg, row.bg));
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

/* A slider bar for a 0-100 value in a row, from BAR_X. */
static void draw_bar(int screen_row, int value, uint16_t color)
{
    int y = screen_row * FH + FH / 4;
    int height = FH / 2;
    int position = BAR_X + value * BAR_WIDTH / 100;
    ESP_ERROR_CHECK(cyd_fill_rect(BAR_X, y, BAR_WIDTH, height, COLOR_TRACK));
    if (position > BAR_X) {
        ESP_ERROR_CHECK(cyd_fill_rect(BAR_X, y, position - BAR_X, height, color));
    }
    int thumb = position - 3 < BAR_X ? BAR_X : position - 3;
    if (thumb + 6 > BAR_X + BAR_WIDTH) {
        thumb = BAR_X + BAR_WIDTH - 6;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(thumb, y - 6, 6, height + 12, CYD_COLOR_WHITE));
}

static void draw_text_row(int screen_row, const char *text, uint16_t fg)
{
    row_t row;
    clear_row(&row, fg);
    memcpy(row.text, text, strnlen(text, COLUMNS));
    ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row.text, row.fg, row.bg));
}

static const span_t RACK_LAYER_SPAN = {LABEL_COLUMNS, COLUMNS - LABEL_COLUMNS};
static const span_t RACK_TOGGLE_SPAN = {0, 4};
static const span_t SAMPLE_SAVE_SPAN = {LABEL_COLUMNS, 8};
static const span_t SAMPLE_PLAY_SPAN = {LABEL_COLUMNS + 9, COLUMNS - LABEL_COLUMNS - 9};

/* A number standing for what a custom page shows. */
static uint32_t custom_signature(void)
{
    uint32_t sig = 2166136261u;
    const psynth_rack_t *rack = psynth_rack();
    const uint8_t *bytes = (const uint8_t *)rack;
    for (size_t i = 0; i < sizeof(*rack); ++i) {
        sig = (sig ^ bytes[i]) * 16777619u;
    }
    for (int i = 0; i < PSYNTH_SAMPLES; ++i) {
        sig = (sig ^ (uint32_t)(psynth_sample_seconds(i) * 100.0f)) * 16777619u;
    }
    for (const char *c = sample_status; *c != '\0'; ++c) {
        sig = (sig ^ (uint8_t)*c) * 16777619u;
    }
    sig ^= (uint32_t)(chosen_sample + 7) * 977u + (uint32_t)slot * 31u;
    return sig;
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
    ESP_ERROR_CHECK(cyd_draw_cells(FIRST_PARAM_ROW, row.text, row.fg, row.bg));
    for (int i = 0; i < PSYNTH_SLOTS; ++i) {
        int screen_row = FIRST_PARAM_ROW + 1 + i;
        clear_row(&row, CYD_COLOR_WHITE);
        char name[8];
        snprintf(name, sizeof(name), "P%d", i + 1);
        put_span(&row, RACK_TOGGLE_SPAN, name, rack->on[i] ? CYD_COLOR_BLACK : CYD_COLOR_GRAY,
                 rack->on[i] ? patch_color() : CYD_COLOR_BUTTON_DISABLED);
        char level[8];
        snprintf(level, sizeof(level), "%4d", rack->level[i]);
        memcpy(row.text + 4, level, 4);
        ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row.text, row.fg, row.bg));
        draw_bar(screen_row, rack->level[i], rack->on[i] ? patch_color() : COLOR_ZERO);
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
        ESP_ERROR_CHECK(cyd_draw_cells(screen_row, row.text, row.fg, row.bg));
    }
    draw_text_row(FIRST_PARAM_ROW + 4, sample_status, CYD_COLOR_YELLOW);
    draw_text_row(FIRST_PARAM_ROW + 5, " Samples play as PATCH styles S1-S4; ROLL: INST", CYD_COLOR_GRAY);
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

static void set_slot(int new_slot)
{
    keys_all_notes_off();
    memset(finger_notes, 0xFF, sizeof(finger_notes));
    slot = new_slot;
    update_test_instrument();
    psynth_set_scope_slot(slot);
    init_armed = false;
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
        page = (page + PAGES - 1) % PAGES;
        init_armed = false;
        update_test_instrument();
    } else if (in_span(NEXT_SPAN, column)) {
        page = (page + 1) % PAGES;
        init_armed = false;
        update_test_instrument();
    } else if (in_span(INIT_SPAN, column)) {
        if (init_armed) {
            psynth_reset(slot);
            init_armed = false;
            drawn_page = -1; /* every value may have changed */
        } else {
            init_armed = true;
        }
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
    keys_all_notes_off();
    memset(finger_notes, 0xFF, sizeof(finger_notes));
    update_test_instrument();
}

static void tap_param(int index, int x)
{
    const page_t *p = &pages[page];
    if (p->kind == PAGE_RACK) {
        tap_rack(index, x);
        return;
    }
    if (p->kind == PAGE_SAMPLES) {
        tap_samples(index, x);
        return;
    }
    if (index >= p->count) {
        return;
    }
    const param_t *param = &p->params[index];
    if (param->choices != NULL && x >= LABEL_COLUMNS * FW) {
        int value = get_value(param) + 1;
        set_value(param, value > param->max ? param->min : value);
    }
}

/* Fingers held on the sliders set them, so a drag sweeps a value. */
static void follow_sliders(void)
{
    cyd_touch_point_t held[5];
    int count = cyd_touch_held(held, 5);
    const page_t *p = &pages[page];
    dragging = false;
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
        if (index < 0 || index >= p->count || held[i].x < BAR_X - FW) {
            continue;
        }
        dragging = true;
        const param_t *param = &p->params[index];
        if (param->choices != NULL) {
            continue;
        }
        int span = param->max - param->min;
        int value = param->min + ((held[i].x - BAR_X) * span + BAR_WIDTH / 2) / BAR_WIDTH;
        if (param->min < 0 && value != 0 && abs(value) <= span / 24) {
            value = 0; /* snaps to the middle */
        }
        set_value(param, value);
    }
}

/* ---- The screen ----------------------------------------------------------------------------- */

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
                tap_param(row - FIRST_PARAM_ROW, point.x);
            }
        }
        follow_sliders();

        draw_header();
        draw_page_row();
        bool all = page != drawn_page || slot != drawn_slot;
        if (pages[page].kind != PAGE_PARAMS) {
            uint32_t sig = custom_signature();
            if (all || sig != drawn_custom) {
                if (all) {
                    ESP_ERROR_CHECK(cyd_fill_rect(0, FIRST_PARAM_ROW * FH, CYD_LCD_WIDTH,
                                                  PARAM_ROWS * FH, CYD_COLOR_BLACK));
                }
                if (pages[page].kind == PAGE_RACK) {
                    draw_rack();
                } else {
                    draw_samples();
                }
                drawn_custom = sig;
            }
        } else {
            for (int index = 0; index < PARAM_ROWS; ++index) {
                int value = index < pages[page].count ? get_value(&pages[page].params[index]) : -9999;
                if (all || value != drawn_values[index]) {
                    draw_param(index);
                    drawn_values[index] = value;
                }
            }
        }
        drawn_page = page;
        drawn_slot = slot;
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
