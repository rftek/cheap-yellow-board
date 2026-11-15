/* ODIN S3's screens, 800x480.

   Play screen:
     row 0       < patch name >, MENU
     row 1       the category (tap to browse), OCT - and +, the keys' range
     row 2       voices, render load, unison and play mode
     controls    BEND and MOD strips, Odin's XY pad, and what the patch uses
     the rest    25 keys, two octaves; lower on a key plays louder

   The keys, strips and pad are played from the touch task (on_fingers),
   which sends notes and controls to the audio task at once; the UI task
   only draws. Taps on the rows above go to the UI task's queue.

   The browser lists a category's presets, ten to a page; the menu holds
   the sound output, volume, tempo and AirPlay delay. */

#include "ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cyd_display.h"
#include "engine.h"
#include "music_out.h"
#include "nvs.h"
#include "patch.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

_Static_assert(CYD_LCD_WIDTH == 800 && CYD_TEXT_ROWS == 12,
               "ODIN S3 is laid out for the 800x480 Waveshare S3 board");

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS
#define SW CYD_SMALL_FONT_WIDTH
#define SH CYD_SMALL_FONT_HEIGHT

#define HEADER_ROWS 3
#define CONTROLS_TOP (HEADER_ROWS * FH + 4)
#define CONTROLS_BOTTOM 274
#define CONTROLS_HEIGHT (CONTROLS_BOTTOM - CONTROLS_TOP)
#define STRIP_WIDTH 60
#define BEND_X 4
#define MOD_X 72
#define XY_X 140
#define XY_WIDTH 200
#define INFO_X 356
#define LABEL_HEIGHT (SH + 4)

#define KEYS 25
#define WHITE_KEYS 15
#define KEYS_TOP 280
#define KEYS_HEIGHT (CYD_LCD_HEIGHT - KEYS_TOP)
#define BLACK_WIDTH 34
#define BLACK_HEIGHT (KEYS_HEIGHT * 3 / 5)
#define LOWEST_BASE 24
#define HIGHEST_BASE 84
#define DEFAULT_BASE 48

#define REFRESH_MS 10
#define MAX_FINGER_IDS 16
#define VOLUME_STEP 10
#define TEMPO_STEP 5

#define NVS_NAMESPACE "odin_s3"
#define NVS_PRESET "preset"

typedef struct {
    int column;
    int width;
} span_t;

static const span_t PREV_SPAN = {0, 3};
static const span_t NAME_SPAN = {3, 16};
static const span_t NEXT_SPAN = {19, 3};
static const span_t MENU_SPAN = {22, 4};
static const span_t CATEGORY_SPAN = {0, 13};
static const span_t OCT_DOWN_SPAN = {14, 4};
static const span_t RANGE_SPAN = {18, 4};
static const span_t OCT_UP_SPAN = {22, 4};

static uint16_t color_accent;   /* Odin's teal */
static uint16_t color_panel;    /* dark gray */
static uint16_t color_track;
static uint16_t color_header;

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

static row_t drawn_rows[HEADER_ROWS];

/* ---- Patches ---------------------------------------------------------------- */

static int preset_index;

static const odin_patch_t *current_patch(void)
{
    return &odin_presets[preset_index];
}

static void select_preset(int index, bool save)
{
    if (index < 0) {
        index = odin_preset_count - 1;
    } else if (index >= odin_preset_count) {
        index = 0;
    }
    preset_index = index;
    audio_load_patch(current_patch());
    if (save) {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_i32(nvs, NVS_PRESET, index);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
}

static int saved_preset(void)
{
    int32_t index = -1;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_i32(nvs, NVS_PRESET, &index);
        nvs_close(nvs);
    }
    if (index >= 0 && index < odin_preset_count) {
        return index;
    }
    for (int i = 0; i < odin_preset_count; ++i) {
        if (odin_presets[i].category == 2) { /* the first bass */
            return i;
        }
    }
    return 0;
}

/* ---- Keys, strips and pad ----------------------------------------------------- */

typedef struct {
    int x;
    int width;
    bool black;
} piano_key_t;

static piano_key_t keys[KEYS];
static volatile int base_note = DEFAULT_BASE;
static volatile bool play_live;
static atomic_uint live_keys;

/* Control values, written by the touch task and drawn by the UI task. */
static volatile float bend_value;
static volatile float mod_value;
static volatile float xy_x, xy_y;

static void layout_keys(void)
{
    static const int white_of[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    for (int key = 0; key < KEYS; ++key) {
        int in_octave = key % 12;
        bool black = in_octave == 1 || in_octave == 3 || in_octave == 6 || in_octave == 8 ||
                     in_octave == 10;
        int white = (key / 12) * 7 + white_of[in_octave];
        if (black) {
            int line = (white + 1) * CYD_LCD_WIDTH / WHITE_KEYS;
            keys[key] = (piano_key_t){line - BLACK_WIDTH / 2, BLACK_WIDTH, true};
        } else {
            int x = white * CYD_LCD_WIDTH / WHITE_KEYS;
            int next = (white + 1) * CYD_LCD_WIDTH / WHITE_KEYS;
            keys[key] = (piano_key_t){x, next - x, false};
        }
    }
}

static int key_at(int x, int y)
{
    if (y < KEYS_TOP) {
        return -1;
    }
    if (y < KEYS_TOP + BLACK_HEIGHT) {
        for (int key = 0; key < KEYS; ++key) {
            if (keys[key].black && x >= keys[key].x && x < keys[key].x + keys[key].width) {
                return key;
            }
        }
    }
    for (int key = 0; key < KEYS; ++key) {
        if (!keys[key].black && x >= keys[key].x && x < keys[key].x + keys[key].width) {
            return key;
        }
    }
    return -1;
}

/* Lower on a key is louder, as a key pressed further from its pivot. */
static int velocity_at(int key, int y)
{
    int height = keys[key].black ? BLACK_HEIGHT : KEYS_HEIGHT;
    int v = 50 + 77 * (y - KEYS_TOP) / height;
    return v < 1 ? 1 : v > 127 ? 127 : v;
}

typedef enum { ZONE_NONE, ZONE_KEYS, ZONE_BEND, ZONE_MOD, ZONE_XY } zone_t;

static zone_t zone_at(int x, int y)
{
    if (y >= KEYS_TOP) {
        return ZONE_KEYS;
    }
    if (y < CONTROLS_TOP || y >= CONTROLS_BOTTOM) {
        return ZONE_NONE;
    }
    if (x >= BEND_X && x < BEND_X + STRIP_WIDTH) {
        return ZONE_BEND;
    }
    if (x >= MOD_X && x < MOD_X + STRIP_WIDTH) {
        return ZONE_MOD;
    }
    if (x >= XY_X && x < XY_X + XY_WIDTH) {
        return ZONE_XY;
    }
    return ZONE_NONE;
}

typedef struct {
    bool down;
    uint8_t zone;
    int8_t key;
    uint8_t note;
} finger_t;

static finger_t fingers[MAX_FINGER_IDS];
static uint8_t holders[128];

static void finger_press(finger_t *finger, int key, int y)
{
    finger->key = (int8_t)key;
    if (key < 0) {
        return;
    }
    finger->note = (uint8_t)(base_note + key);
    if (holders[finger->note]++ == 0) {
        audio_note_on(finger->note, velocity_at(key, y));
    }
}

static void finger_lift(finger_t *finger)
{
    if (finger->key >= 0 && holders[finger->note] > 0 && --holders[finger->note] == 0) {
        audio_note_off(finger->note);
    }
    finger->key = -1;
}

static float control_fraction(int y)
{
    float f = (float)(CONTROLS_BOTTOM - 1 - y) / (float)(CONTROLS_HEIGHT - LABEL_HEIGHT - 1);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static void finger_control(finger_t *finger, int x, int y)
{
    switch (finger->zone) {
    case ZONE_BEND: {
        float value = control_fraction(y) * 2.0f - 1.0f;
        if (value != bend_value) {
            bend_value = value;
            audio_set_pitchbend(value);
        }
        break;
    }
    case ZONE_MOD: {
        float value = control_fraction(y);
        if (value != mod_value) {
            mod_value = value;
            audio_set_modwheel(value);
        }
        break;
    }
    case ZONE_XY: {
        float fx = (float)(x - XY_X) / (float)(XY_WIDTH - 1);
        fx = fx < 0.0f ? 0.0f : fx > 1.0f ? 1.0f : fx;
        float fy = control_fraction(y);
        if (fx != xy_x || fy != xy_y) {
            xy_x = fx;
            xy_y = fy;
            audio_set_xy(fx, fy);
        }
        break;
    }
    default:
        break;
    }
}

static void on_fingers(const cyd_touch_fingers_t *report, void *context)
{
    (void)context;
    if (!play_live) {
        memset(fingers, 0, sizeof(fingers));
        memset(holders, 0, sizeof(holders));
        atomic_store(&live_keys, 0);
        return;
    }
    bool seen[MAX_FINGER_IDS] = {false};
    for (int i = 0; i < report->count; ++i) {
        int id = report->ids[i] % MAX_FINGER_IDS;
        finger_t *finger = &fingers[id];
        int x = report->points[i].x;
        int y = report->points[i].y;
        seen[id] = true;
        if (!finger->down) {
            finger->down = true;
            finger->zone = (uint8_t)zone_at(x, y);
            finger->key = -1;
            if (finger->zone == ZONE_KEYS) {
                finger_press(finger, key_at(x, y), y);
            }
        } else if (finger->zone == ZONE_KEYS) {
            int key = key_at(x, y);
            if (key != finger->key) {
                finger_lift(finger);
                finger_press(finger, key, y);
            }
        }
        finger_control(finger, x, y);
    }
    unsigned mask = 0;
    for (int id = 0; id < MAX_FINGER_IDS; ++id) {
        finger_t *finger = &fingers[id];
        if (finger->down && !seen[id]) {
            if (finger->zone == ZONE_KEYS) {
                finger_lift(finger);
            } else if (finger->zone == ZONE_BEND) {
                bend_value = 0.0f; /* the bend springs back */
                audio_set_pitchbend(0.0f);
            }
            finger->down = false;
        }
        if (finger->down && finger->zone == ZONE_KEYS && finger->key >= 0) {
            int key = finger->note - base_note;
            if (key >= 0 && key < KEYS) {
                mask |= 1u << key;
            }
        }
    }
    atomic_store(&live_keys, mask);
}

/* The play surface is handled by on_fingers; taps above it go to the UI. */
static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    return play_live && point->y >= CONTROLS_TOP;
}

/* ---- Drawing -------------------------------------------------------------------- */

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

static void clear_row(row_t *row, uint16_t background)
{
    memset(row->text, ' ', COLUMNS);
    row->text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row->fg[column] = CYD_COLOR_WHITE;
        row->bg[column] = background;
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

static void draw_row(int index, const row_t *row)
{
    if (memcmp(&drawn_rows[index], row, sizeof(*row)) == 0) {
        return;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(index, row->text, row->fg, row->bg));
    drawn_rows[index] = *row;
}

static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B"};

static void refresh_header(void)
{
    const odin_patch_t *p = current_patch();
    row_t row;
    clear_row(&row, color_header);
    put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, NAME_SPAN, p->name, color_accent, color_header);
    put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(0, &row);

    clear_row(&row, CYD_COLOR_BLACK);
    char label[COLUMNS + 1];
    snprintf(label, sizeof(label), "%s", odin_categories[p->category]);
    for (char *c = label; *c; ++c) {
        *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    }
    put_span(&row, CATEGORY_SPAN, label, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, OCT_DOWN_SPAN, "OCT-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    snprintf(label, sizeof(label), "C%d", base_note / 12 - 1);
    put_span(&row, RANGE_SPAN, label, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, OCT_UP_SPAN, "OCT+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(1, &row);

    static const char *const modes[3] = {"LEGATO", "POLY", "RETRIG"};
    int average, worst;
    audio_load(&average, &worst);
    int unison = p->unison < 1 ? 1 : p->unison > ENGINE_UNISON_MAX ? ENGINE_UNISON_MAX : p->unison;
    clear_row(&row, CYD_COLOR_BLACK);
    snprintf(row.text, sizeof(row.text), " %d/%d VOICES  %3d%% CPU  %s%s", audio_voices(), ENGINE_VOICES,
             average, modes[p->play_mode < 3 ? p->play_mode : 1],
             unison > 1 ? (unison == 2 ? " x2" : unison == 3 ? " x3" : " x4") : "");
    row.text[strlen(row.text)] = ' ';
    row.text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row.fg[column] = worst > 90 ? CYD_COLOR_RED : CYD_COLOR_GRAY;
    }
    draw_row(2, &row);
}

static void draw_label(int x, const char *text)
{
    ESP_ERROR_CHECK(cyd_draw_small_text(x + 2, CONTROLS_TOP, text, CYD_COLOR_GRAY, CYD_COLOR_BLACK));
}

#define PAD_TOP (CONTROLS_TOP + LABEL_HEIGHT)
#define PAD_HEIGHT (CONTROLS_BOTTOM - PAD_TOP)

static float drawn_bend = -2.0f, drawn_mod = -2.0f, drawn_x = -2.0f, drawn_y = -2.0f;

static void draw_bend(void)
{
    float value = bend_value;
    if (value == drawn_bend) {
        return;
    }
    drawn_bend = value;
    int middle = PAD_TOP + PAD_HEIGHT / 2;
    int level = middle - (int)(value * (PAD_HEIGHT / 2 - 2));
    ESP_ERROR_CHECK(cyd_fill_rect(BEND_X, PAD_TOP, STRIP_WIDTH, PAD_HEIGHT, color_track));
    int top = level < middle ? level : middle;
    int height = level < middle ? middle - level : level - middle;
    ESP_ERROR_CHECK(cyd_fill_rect(BEND_X + 6, top, STRIP_WIDTH - 12, height + 2, color_accent));
    ESP_ERROR_CHECK(cyd_fill_rect(BEND_X, middle, STRIP_WIDTH, 1, CYD_COLOR_GRAY));
}

static void draw_mod(void)
{
    float value = mod_value;
    if (value == drawn_mod) {
        return;
    }
    drawn_mod = value;
    int height = (int)(value * (PAD_HEIGHT - 2));
    ESP_ERROR_CHECK(cyd_fill_rect(MOD_X, PAD_TOP, STRIP_WIDTH, PAD_HEIGHT - height, color_track));
    ESP_ERROR_CHECK(cyd_fill_rect(MOD_X, PAD_TOP + PAD_HEIGHT - height, STRIP_WIDTH, height, color_accent));
}

static void draw_xy(void)
{
    float x = xy_x, y = xy_y;
    if (x == drawn_x && y == drawn_y) {
        return;
    }
    drawn_x = x;
    drawn_y = y;
    ESP_ERROR_CHECK(cyd_fill_rect(XY_X, PAD_TOP, XY_WIDTH, PAD_HEIGHT, color_track));
    int px = XY_X + (int)(x * (XY_WIDTH - 1));
    int py = PAD_TOP + PAD_HEIGHT - 1 - (int)(y * (PAD_HEIGHT - 1));
    ESP_ERROR_CHECK(cyd_fill_rect(XY_X, py, XY_WIDTH, 1, CYD_COLOR_GRAY));
    ESP_ERROR_CHECK(cyd_fill_rect(px, PAD_TOP, 1, PAD_HEIGHT, CYD_COLOR_GRAY));
    int dot_x = px - 7 < XY_X ? XY_X : px + 7 > XY_X + XY_WIDTH ? XY_X + XY_WIDTH - 14 : px - 7;
    int dot_y = py - 7 < PAD_TOP ? PAD_TOP : py + 7 > PAD_TOP + PAD_HEIGHT ? PAD_TOP + PAD_HEIGHT - 14 : py - 7;
    ESP_ERROR_CHECK(cyd_fill_rect(dot_x, dot_y, 14, 14, color_accent));
}

static const char *osc_text(const odin_osc_patch_t *o, char *buffer, size_t size)
{
    static const char *const analog[4] = {"SAW", "SQUARE", "TRIANGLE", "SINE"};
    switch (o->type) {
    case ODIN_OSC_ANALOG:
        snprintf(buffer, size, "ANALOG %s", analog[o->analog_wave & 3]);
        break;
    case ODIN_OSC_WAVETABLE:
        snprintf(buffer, size, "WAVETABLE %d", o->wavetable + 1);
        break;
    case ODIN_OSC_NOISE:
        snprintf(buffer, size, "NOISE");
        break;
    default:
        snprintf(buffer, size, "-");
        break;
    }
    return buffer;
}

static const char *filter_name(int type)
{
    switch (type) {
    case ODIN_FILTER_LP24: return "LP24";
    case ODIN_FILTER_LP12: return "LP12";
    case ODIN_FILTER_BP24: return "BP24";
    case ODIN_FILTER_BP12: return "BP12";
    case ODIN_FILTER_HP24: return "HP24";
    case ODIN_FILTER_HP12: return "HP12";
    case ODIN_FILTER_SEM12: return "SEM12";
    case ODIN_FILTER_DIODE: return "DIODE";
    case ODIN_FILTER_KORG_LP: return "KORG LP";
    case ODIN_FILTER_KORG_HP: return "KORG HP";
    default: return "-";
    }
}

/* What the patch uses, in small text beside the pad. */
static void draw_info(void)
{
    const odin_patch_t *p = current_patch();
    ESP_ERROR_CHECK(cyd_fill_rect(INFO_X, CONTROLS_TOP, CYD_LCD_WIDTH - INFO_X, CONTROLS_HEIGHT,
                                  CYD_COLOR_BLACK));
    char line[40], part[24];
    int y = CONTROLS_TOP;
    for (int o = 0; o < 3; ++o) {
        snprintf(line, sizeof(line), "OSC%d %s", o + 1, osc_text(&p->osc[o], part, sizeof(part)));
        ESP_ERROR_CHECK(cyd_draw_small_text(INFO_X, y, line,
                                            p->osc[o].type == ODIN_OSC_OFF ? CYD_COLOR_GRAY : CYD_COLOR_WHITE,
                                            CYD_COLOR_BLACK));
        y += SH + 1;
    }
    snprintf(line, sizeof(line), "FIL %s %s %s", filter_name(p->fil[0].type), filter_name(p->fil[1].type),
             filter_name(p->fil[2].type));
    ESP_ERROR_CHECK(cyd_draw_small_text(INFO_X, y, line, CYD_COLOR_WHITE, CYD_COLOR_BLACK));
    y += SH + 1;
    snprintf(line, sizeof(line), "FX%s%s%s%s%s", p->dist_on ? " DIST" : "", p->delay.on ? " DELAY" : "",
             p->phaser.on ? " PHASER" : "", p->flanger.on ? " FLANGER" : "", p->chorus.on ? " CHORUS" : "");
    if (strlen(line) == 2) {
        snprintf(line, sizeof(line), "FX -");
    }
    ESP_ERROR_CHECK(cyd_draw_small_text(INFO_X, y, line, CYD_COLOR_WHITE, CYD_COLOR_BLACK));
    y += SH + 1;
    int rows = 0;
    for (int r = 0; r < ODIN_MOD_ROWS; ++r) {
        rows += p->mod[r].source > 1 && (p->mod[r].dest1 > 1 || p->mod[r].dest2 > 1);
    }
    snprintf(line, sizeof(line), "MOD MATRIX %d ROW%s", rows, rows == 1 ? "" : "S");
    ESP_ERROR_CHECK(cyd_draw_small_text(INFO_X, y, line, CYD_COLOR_GRAY, CYD_COLOR_BLACK));
}

/* ---- Key drawing ------------------------------------------------------------- */

#define KEY_LIVE 1
#define KEY_NOT_DRAWN 0xFF

static uint8_t drawn_keys[KEYS];

static uint16_t key_color(int key, uint8_t state)
{
    if (state != KEY_NOT_DRAWN && (state & KEY_LIVE)) {
        return color_accent;
    }
    return keys[key].black ? 0x18E3 : CYD_COLOR_WHITE;
}

static void draw_key_label(int key, int bottom)
{
    char label[4];
    int note = base_note + key;
    if (keys[key].black) {
        snprintf(label, sizeof(label), "%s", note_names[note % 12]);
    } else {
        snprintf(label, sizeof(label), "%s%d", note_names[note % 12], note / 12 - 1);
    }
    uint8_t state = drawn_keys[key];
    uint16_t fg = (state != KEY_NOT_DRAWN && (state & KEY_LIVE)) ? CYD_COLOR_BLACK
                  : note == 60                                   ? CYD_COLOR_RED
                  : keys[key].black                              ? cyd_rgb(170, 170, 170)
                                                                 : cyd_rgb(90, 90, 90);
    int width = (int)strlen(label) * SW;
    ESP_ERROR_CHECK(cyd_draw_small_text(keys[key].x + (keys[key].width - width) / 2, bottom - SH - 8, label,
                                        fg, key_color(key, state)));
}

static void draw_black(int key)
{
    ESP_ERROR_CHECK(cyd_fill_rect(keys[key].x, KEYS_TOP, keys[key].width, BLACK_HEIGHT,
                                  key_color(key, drawn_keys[key])));
    draw_key_label(key, KEYS_TOP + BLACK_HEIGHT);
}

static void draw_white(int key)
{
    const piano_key_t *k = &keys[key];
    ESP_ERROR_CHECK(cyd_fill_rect(k->x + 1, KEYS_TOP, k->width - 2, KEYS_HEIGHT, key_color(key, drawn_keys[key])));
    draw_key_label(key, CYD_LCD_HEIGHT);
    for (int neighbor = key - 1; neighbor <= key + 1; neighbor += 2) {
        if (neighbor >= 0 && neighbor < KEYS && keys[neighbor].black) {
            draw_black(neighbor);
        }
    }
}

static void refresh_keys(void)
{
    unsigned live = atomic_load(&live_keys);
    for (int key = 0; key < KEYS; ++key) {
        uint8_t state = (live & (1u << key)) ? KEY_LIVE : 0;
        if (state == drawn_keys[key]) {
            continue;
        }
        drawn_keys[key] = state;
        if (keys[key].black) {
            draw_black(key);
        } else {
            draw_white(key);
        }
    }
}

static void full_redraw(void)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    memset(drawn_rows, 0, sizeof(drawn_rows));
    memset(drawn_keys, KEY_NOT_DRAWN, sizeof(drawn_keys));
    drawn_bend = drawn_mod = drawn_x = drawn_y = -2.0f;
    draw_label(BEND_X, "BEND");
    draw_label(MOD_X, "MOD");
    draw_label(XY_X, "X / Y");
    draw_info();
    refresh_header();
    draw_bend();
    draw_mod();
    draw_xy();
    refresh_keys();
}

/* The patch's own wheel and pad positions, as Odin loads them. */
static void take_patch_controls(void)
{
    const odin_patch_t *p = current_patch();
    mod_value = p->modwheel;
    xy_x = p->xy_x;
    xy_y = p->xy_y;
    bend_value = 0.0f;
}

/* ---- Browser ----------------------------------------------------------------- */

#define BROWSER_ROWS 10

static int category_count(int category)
{
    int count = 0;
    for (int i = 0; i < odin_preset_count; ++i) {
        count += odin_presets[i].category == category;
    }
    return count;
}

/* The index of the n-th preset in a category. */
static int category_preset(int category, int n)
{
    for (int i = 0; i < odin_preset_count; ++i) {
        if (odin_presets[i].category == category && n-- == 0) {
            return i;
        }
    }
    return -1;
}

static int step_category(int category, int step)
{
    for (int tries = 0; tries < ODIN_CATEGORIES; ++tries) {
        category = (category + step + ODIN_CATEGORIES) % ODIN_CATEGORIES;
        if (category_count(category) > 0) {
            return category;
        }
    }
    return category;
}

static void draw_browser(int category, int page)
{
    row_t row;
    clear_row(&row, color_header);
    char label[COLUMNS + 1];
    snprintf(label, sizeof(label), "%s", odin_categories[category]);
    put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, NAME_SPAN, label, color_accent, color_header);
    put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, MENU_SPAN, "BACK", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(0, row.text, row.fg, row.bg));

    int count = category_count(category);
    for (int r = 0; r < BROWSER_ROWS; ++r) {
        int n = page * BROWSER_ROWS + r;
        int index = n < count ? category_preset(category, n) : -1;
        if (index < 0) {
            ESP_ERROR_CHECK(cyd_text_row(1 + r, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
            continue;
        }
        snprintf(label, sizeof(label), " %s", odin_presets[index].name);
        bool current = index == preset_index;
        ESP_ERROR_CHECK(cyd_text_row(1 + r, current ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                                     current ? color_accent : (r % 2 ? color_panel : CYD_COLOR_BLACK), label));
    }
    int pages = (count + BROWSER_ROWS - 1) / BROWSER_ROWS;
    clear_row(&row, CYD_COLOR_BLACK);
    if (pages > 1) {
        snprintf(label, sizeof(label), "PAGE %d/%d", page + 1, pages);
        put_span(&row, PREV_SPAN, "<", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
        put_span(&row, (span_t){3, 16}, label, CYD_COLOR_GRAY, CYD_COLOR_BLACK);
        put_span(&row, NEXT_SPAN, ">", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    }
    ESP_ERROR_CHECK(cyd_draw_cells(11, row.text, row.fg, row.bg));
}

/* Lists the presets by category; a tap on one plays it and goes back. */
static void run_browser(QueueHandle_t touch_queue)
{
    int category = current_patch()->category;
    int count = category_count(category);
    int position = 0;
    for (int n = 0; n < count; ++n) {
        if (category_preset(category, n) == preset_index) {
            position = n;
        }
    }
    int page = position / BROWSER_ROWS;
    draw_browser(category, page);
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        int pages = (category_count(category) + BROWSER_ROWS - 1) / BROWSER_ROWS;
        if (row == 0) {
            if (in_span(MENU_SPAN, column)) {
                return;
            }
            if (in_span(PREV_SPAN, column) || in_span(NEXT_SPAN, column)) {
                category = step_category(category, in_span(PREV_SPAN, column) ? -1 : 1);
                page = 0;
                draw_browser(category, page);
            }
        } else if (row == 11) {
            if (pages > 1 && (in_span(PREV_SPAN, column) || in_span(NEXT_SPAN, column))) {
                page = (page + (in_span(PREV_SPAN, column) ? pages - 1 : 1)) % pages;
                draw_browser(category, page);
            }
        } else {
            int n = page * BROWSER_ROWS + row - 1;
            if (n < category_count(category)) {
                select_preset(category_preset(category, n), true);
                take_patch_controls();
                return;
            }
        }
    }
}

/* ---- Menu --------------------------------------------------------------------- */

#define MENU_INFO 1
#define MENU_WIFI 2
#define MENU_OUTPUT 3
#define MENU_VOLUME 4
#define MENU_TEMPO 5
#define MENU_LATENCY 6
#define MENU_ABOUT 8
#define MENU_BACK 11
#define SPLIT_BUTTON_COLUMNS 9

static char status_text[COLUMNS + 1] = " No speaker: silent";
static int tempo;

static void menu_status(const char *status)
{
    snprintf(status_text, sizeof(status_text), "%s", status);
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
}

static void draw_split_row(int index, const char *left, const char *value, const char *right)
{
    row_t row;
    clear_row(&row, CYD_COLOR_BLACK);
    put_span(&row, (span_t){0, SPLIT_BUTTON_COLUMNS}, left, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, (span_t){SPLIT_BUTTON_COLUMNS, COLUMNS - 2 * SPLIT_BUTTON_COLUMNS}, value,
             CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, (span_t){COLUMNS - SPLIT_BUTTON_COLUMNS, SPLIT_BUTTON_COLUMNS}, right, CYD_COLOR_WHITE,
             CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(index, row.text, row.fg, row.bg));
}

static void draw_volume_row(void)
{
    char value[12];
    if (music_out_has_speaker()) {
        snprintf(value, sizeof(value), "%d%%", music_out_get_volume());
    } else {
        snprintf(value, sizeof(value), "SILENT");
    }
    draw_split_row(MENU_VOLUME, "VOL -", value, "VOL +");
}

static void draw_tempo_row(void)
{
    char value[12];
    snprintf(value, sizeof(value), "%d BPM", tempo);
    draw_split_row(MENU_TEMPO, "BPM -5", value, "BPM +5");
}

static void draw_menu(void)
{
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_BLACK, color_accent, " ODIN S3 SETTINGS"));
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
    music_out_draw_wifi_row(MENU_WIFI);
    music_out_draw_output_row(MENU_OUTPUT);
    draw_volume_row();
    draw_tempo_row();
    music_out_draw_latency_row(MENU_LATENCY);
    ESP_ERROR_CHECK(cyd_text_row(7, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " %d ODIN 2 PRESETS", odin_preset_count);
    ESP_ERROR_CHECK(cyd_text_row(MENU_ABOUT, CYD_COLOR_GRAY, CYD_COLOR_BLACK, line));
    ESP_ERROR_CHECK(cyd_text_row(9, CYD_COLOR_GRAY, CYD_COLOR_BLACK, " A port of Odin 2 by"));
    ESP_ERROR_CHECK(cyd_text_row(10, CYD_COLOR_GRAY, CYD_COLOR_BLACK, " TheWaveWarden, GPL-3.0"));
    ESP_ERROR_CHECK(cyd_text_row(MENU_BACK, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " BACK TO KEYS"));
}

static void run_menu(QueueHandle_t touch_queue)
{
    int saved_tempo = tempo;
    draw_menu();
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (music_out_check_lost()) {
                menu_status(" Speaker disconnected");
                music_out_draw_output_row(MENU_OUTPUT);
                draw_volume_row();
            }
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        bool left = column < SPLIT_BUTTON_COLUMNS;
        bool right = column >= COLUMNS - SPLIT_BUTTON_COLUMNS;
        char status[COLUMNS + 1];
        switch (row) {
        case 0:
        case MENU_BACK:
            if (tempo != saved_tempo) {
                music_out_save_tempo((uint16_t)tempo);
            }
            return;
        case MENU_WIFI:
            if (music_out_tap_wifi(status, sizeof(status))) {
                snprintf(status_text, sizeof(status_text), "%s", status);
                draw_menu();
                xQueueReset(touch_queue);
            } else {
                menu_status(status);
            }
            break;
        case MENU_OUTPUT:
            music_out_pick_speaker(status, sizeof(status));
            snprintf(status_text, sizeof(status_text), "%s", status);
            draw_menu();
            xQueueReset(touch_queue);
            break;
        case MENU_VOLUME:
            if ((left || right) && music_out_has_speaker()) {
                music_out_set_volume(music_out_get_volume() + (left ? -VOLUME_STEP : VOLUME_STEP));
                draw_volume_row();
            }
            break;
        case MENU_TEMPO:
            if (left || right) {
                tempo += left ? -TEMPO_STEP : TEMPO_STEP;
                tempo = tempo < MUSIC_TEMPO_MIN ? MUSIC_TEMPO_MIN : tempo > MUSIC_TEMPO_MAX ? MUSIC_TEMPO_MAX : tempo;
                audio_set_bpm(tempo);
                draw_tempo_row();
                menu_status(" For synced LFOs and effects");
            }
            break;
        case MENU_LATENCY:
            music_out_next_latency_setting();
            music_out_draw_latency_row(MENU_LATENCY);
            menu_status(" Used from the next connect");
            break;
        default:
            break;
        }
    }
}

/* ---- Play screen --------------------------------------------------------------- */

static void enter_play(QueueHandle_t touch_queue)
{
    full_redraw();
    xQueueReset(touch_queue);
    play_live = true;
    cyd_touch_set_fast(true);
}

static void leave_play(void)
{
    play_live = false;
    cyd_touch_set_fast(false);
    audio_all_notes_off();
}

static void set_base(int base)
{
    base = base < LOWEST_BASE ? LOWEST_BASE : base > HIGHEST_BASE ? HIGHEST_BASE : base;
    if (base != base_note) {
        audio_all_notes_off();
        base_note = base;
        memset(drawn_keys, KEY_NOT_DRAWN, sizeof(drawn_keys));
    }
}

static void change_preset(int step)
{
    select_preset(preset_index + step, true);
    take_patch_controls();
    draw_info();
}

void ui_run(QueueHandle_t touch_queue, int bpm)
{
    color_accent = cyd_rgb(60, 200, 210);
    color_panel = cyd_rgb(24, 26, 30);
    color_track = cyd_rgb(36, 40, 46);
    color_header = cyd_rgb(30, 33, 38);
    layout_keys();
    tempo = bpm;
    audio_set_bpm(bpm);
    select_preset(saved_preset(), false);
    take_patch_controls();

    cyd_touch_set_tap_callback(on_tap, NULL);
    cyd_touch_set_fingers_callback(on_fingers, NULL);
    enter_play(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        while (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            if (point.y >= CONTROLS_TOP) {
                continue; /* came before the keys went live */
            }
            int row = point.y / FH;
            int column = point.x / FW;
            if (row == 0 && in_span(MENU_SPAN, column)) {
                leave_play();
                run_menu(touch_queue);
                enter_play(touch_queue);
                break;
            }
            if ((row == 0 && in_span(NAME_SPAN, column)) || (row == 1 && in_span(CATEGORY_SPAN, column))) {
                leave_play();
                run_browser(touch_queue);
                enter_play(touch_queue);
                break;
            }
            if (row == 0 && (in_span(PREV_SPAN, column) || in_span(NEXT_SPAN, column))) {
                change_preset(in_span(PREV_SPAN, column) ? -1 : 1);
            } else if (row == 1 && in_span(OCT_DOWN_SPAN, column)) {
                set_base(base_note - 12);
            } else if (row == 1 && in_span(OCT_UP_SPAN, column)) {
                set_base(base_note + 12);
            }
        }
        if (music_out_check_lost()) {
            snprintf(status_text, sizeof(status_text), " Speaker disconnected");
        }
        refresh_header();
        draw_bend();
        draw_mod();
        draw_xy();
        refresh_keys();
        if (wait == 0) {
            vTaskDelay(1); /* taps came in: still let the idle task run */
        }
    }
}
