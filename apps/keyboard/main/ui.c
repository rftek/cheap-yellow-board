/* Keyboard screen, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 120 +, MET, MENU
     row 1       sound: VOICE (piano, organ, ...), its STYLE, UNDO
     row 2       OCT - / range / OCT +, SUS (sustain pedal)
     row 3       the loop: length, the bar and beat the speaker is on, notes
     strip       loop progress, as heard
     the rest    25 keys, C to C two octaves up, each with its note name

   Keys are played from the touch task (on_fingers), which sends each note
   to the audio task at once; this task only draws. A key lights in the
   voice's color under a finger, and in blue while the loop plays it, at
   the moment the speaker does. */

#include "ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cyd_display.h"
#include "music_out.h"
#include "synth.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

_Static_assert(CYD_LCD_WIDTH == 800 && CYD_TEXT_ROWS == 12,
               "Keyboard is laid out for the 800x480 Waveshare S3 board");

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS

#define KEYS 25 /* two octaves, C to C */
#define WHITE_KEYS 15
#define LOOP_ROW 3
#define PROGRESS_TOP (4 * FH)
#define PROGRESS_HEIGHT 6
#define KEYS_TOP (PROGRESS_TOP + PROGRESS_HEIGHT + 4)
#define KEYS_HEIGHT (CYD_LCD_HEIGHT - KEYS_TOP)
#define BLACK_WIDTH 34
#define BLACK_HEIGHT (KEYS_HEIGHT * 3 / 5)

#define LOWEST_BASE 36  /* C2 */
#define HIGHEST_BASE 72 /* C5, so the top key is C7 */
#define DEFAULT_BASE 48 /* C3 to C5 */

#define REFRESH_MS 10
#define TEMPO_SAVE_MS 2000
#define MAX_FINGER_IDS 16
#define NO_KEY -1
#define IGNORED -2 /* a finger that came down off the keys */

#define COLOR_WHITE_KEY CYD_COLOR_WHITE
#define COLOR_BLACK_KEY 0x18E3
#define COLOR_PLAYING 0x0560
#define COLOR_PROGRESS_BACK 0x2104

typedef struct {
    int column;
    int width;
} span_t;

/* Row 0. */
static const span_t PLAY_SPAN = {0, 4};
static const span_t REC_SPAN = {5, 3};
static const span_t BPM_DOWN_SPAN = {9, 2};
static const span_t BPM_SPAN = {11, 3};
static const span_t BPM_UP_SPAN = {14, 2};
static const span_t MET_SPAN = {17, 4};
static const span_t MENU_SPAN = {22, 4};
/* Row 1. */
static const span_t VOICE_SPAN = {0, 7};
static const span_t STYLE_SPAN = {8, 11};
static const span_t UNDO_SPAN = {20, 6};
/* Row 2. */
static const span_t OCTAVE_DOWN_SPAN = {0, 6};
static const span_t OCTAVE_SPAN = {7, 7};
static const span_t OCTAVE_UP_SPAN = {15, 6};
static const span_t SUS_SPAN = {22, 4};

#define TOAST_MS 2500

typedef struct {
    int x;
    int width;
    bool black;
} piano_key_t;

static piano_key_t keys[KEYS];
static uint16_t voice_colors[VOICE_KINDS];
static uint16_t loop_white;
static uint16_t loop_black;

/* Read by the touch task. */
static volatile int base_note = DEFAULT_BASE;
static volatile int instrument; /* synth_instrument(voice_kind, its style) */
static int voice_kind = VOICE_PIANO;
static int styles[VOICE_KINDS]; /* the style picked for each voice */
static volatile bool keys_live;
static atomic_uint live_keys; /* bit per key under a finger */

static bool sustain;
static uint8_t loop_sounding[128]; /* loop notes the speaker is playing */
static bool speaker_lost_notice;
static uint16_t saved_bpm;
static uint16_t changed_bpm;
static TickType_t bpm_changed_at;

/* What is drawn now, so a refresh redraws only changes. */
typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

static row_t drawn_rows[LOOP_ROW + 1];
/* A line about the style just picked, shown for a moment on the loop row. */
static char toast[COLUMNS + 1];
static TickType_t toast_until;
static uint8_t drawn_keys[KEYS];
static int drawn_progress;
static uint16_t drawn_progress_color;

#define KEY_LIVE 1
#define KEY_LOOP 2
#define KEY_NOT_DRAWN 0xFF

/* ---- Keys ----------------------------------------------------------------- */

static void layout_keys(void)
{
    static const int white_of[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    for (int key = 0; key < KEYS; ++key) {
        int octave = key / 12;
        int in_octave = key % 12;
        bool black = in_octave == 1 || in_octave == 3 || in_octave == 6 || in_octave == 8 ||
                     in_octave == 10;
        int white = octave * 7 + white_of[in_octave];
        if (black) {
            /* Centered on the line after its white key. */
            int line = (white + 1) * CYD_LCD_WIDTH / WHITE_KEYS;
            keys[key] = (piano_key_t){line - BLACK_WIDTH / 2, BLACK_WIDTH, true};
        } else {
            int x = white * CYD_LCD_WIDTH / WHITE_KEYS;
            int next = (white + 1) * CYD_LCD_WIDTH / WHITE_KEYS;
            keys[key] = (piano_key_t){x, next - x, false};
        }
    }
}

/* The key under a point, or NO_KEY. Black keys sit on top of the whites. */
static int key_at(int x, int y)
{
    if (y < KEYS_TOP) {
        return NO_KEY;
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
    return NO_KEY;
}

/* ---- Fingers: runs in the touch task ------------------------------------- */

typedef struct {
    bool down;
    int8_t key; /* NO_KEY between keys, IGNORED if it came down elsewhere */
    uint8_t note;
    uint8_t instrument;
} finger_t;

static finger_t fingers[MAX_FINGER_IDS];
static uint8_t holders[128]; /* fingers holding each note */

static void finger_press(finger_t *finger, int key)
{
    finger->key = (int8_t)key;
    if (key < 0) {
        return;
    }
    finger->note = (uint8_t)(base_note + key);
    finger->instrument = (uint8_t)instrument;
    if (holders[finger->note]++ == 0) {
        audio_note_on(finger->note, finger->instrument);
    }
}

static void finger_lift(finger_t *finger)
{
    if (finger->key >= 0 && holders[finger->note] > 0 && --holders[finger->note] == 0) {
        audio_note_off(finger->note, finger->instrument);
    }
    finger->key = NO_KEY;
}

/* Called with every report from the panel: a new finger on a key plays
   it, a finger sliding onto another key moves the note there, and a
   lifted finger ends its note. */
static void on_fingers(const cyd_touch_fingers_t *report, void *context)
{
    (void)context;
    if (!keys_live) {
        memset(fingers, 0, sizeof(fingers));
        memset(holders, 0, sizeof(holders));
        atomic_store(&live_keys, 0);
        return;
    }
    bool seen[MAX_FINGER_IDS] = {false};
    for (int i = 0; i < report->count; ++i) {
        int id = report->ids[i] % MAX_FINGER_IDS;
        finger_t *finger = &fingers[id];
        int key = key_at(report->points[i].x, report->points[i].y);
        seen[id] = true;
        if (!finger->down) {
            finger->down = true;
            if (key == NO_KEY) {
                finger->key = IGNORED;
            } else {
                finger_press(finger, key);
            }
        } else if (finger->key != IGNORED && key != finger->key) {
            finger_lift(finger);
            finger_press(finger, key);
        }
    }
    unsigned mask = 0;
    for (int id = 0; id < MAX_FINGER_IDS; ++id) {
        finger_t *finger = &fingers[id];
        if (finger->down && !seen[id]) {
            finger_lift(finger);
            finger->down = false;
        }
        if (finger->down && finger->key >= 0) {
            int key = finger->note - base_note;
            if (key >= 0 && key < KEYS) {
                mask |= 1u << key;
            }
        }
    }
    atomic_store(&live_keys, mask);
}

/* Taps on the keys are notes, handled by on_fingers; the rest go to the
   UI task's queue. */
static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    return keys_live && point->y >= KEYS_TOP;
}

/* ---- Drawing -------------------------------------------------------------- */

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

static void clear_row(row_t *row)
{
    memset(row->text, ' ', COLUMNS);
    row->text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row->fg[column] = CYD_COLOR_WHITE;
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

static void draw_row(int index, const row_t *row)
{
    row_t *drawn = &drawn_rows[index];
    if (memcmp(drawn, row, sizeof(*row)) == 0) {
        return;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(index, row->text, row->fg, row->bg));
    *drawn = *row;
}

static void refresh_transport(const looper_state_t *state)
{
    row_t row;
    clear_row(&row);
    put_span(&row, PLAY_SPAN, state->playing ? "STOP" : "PLAY", CYD_COLOR_WHITE,
             state->playing ? COLOR_PLAYING : CYD_COLOR_BUTTON);
    uint16_t rec = !state->recording ? CYD_COLOR_BUTTON
                   : state->counting_in ? cyd_rgb(200, 120, 0)
                                        : CYD_COLOR_RED;
    put_span(&row, REC_SPAN, "REC", CYD_COLOR_WHITE, rec);
    put_span(&row, BPM_DOWN_SPAN, "-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char number[8];
    snprintf(number, sizeof(number), "%u", audio_settings.bpm);
    put_span(&row, BPM_SPAN, number, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, BPM_UP_SPAN, "+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    bool met = audio_settings.metronome;
    put_span(&row, MET_SPAN, "MET", met ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             met ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    draw_row(0, &row);
}

static void refresh_sound(const looper_state_t *state)
{
    row_t row;
    clear_row(&row);
    put_span(&row, VOICE_SPAN, synth_voice_name(voice_kind), CYD_COLOR_BLACK, voice_colors[voice_kind]);
    put_span(&row, STYLE_SPAN, synth_style_name(instrument), CYD_COLOR_WHITE,
             synth_style_count(voice_kind) > 1 ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, UNDO_SPAN, "UNDO", state->notes > 0 ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             state->notes > 0 ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    draw_row(1, &row);

    clear_row(&row);
    bool can_down = base_note > LOWEST_BASE;
    bool can_up = base_note < HIGHEST_BASE;
    put_span(&row, OCTAVE_DOWN_SPAN, "OCT -", can_down ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_down ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    char range[12];
    int octave = base_note / 12 - 1;
    snprintf(range, sizeof(range), "C%d-C%d", octave, octave + 2);
    put_span(&row, OCTAVE_SPAN, range, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, OCTAVE_UP_SPAN, "OCT +", can_up ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_up ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, SUS_SPAN, "SUS", sustain ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             sustain ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON);
    draw_row(2, &row);
}

/* Where the speaker is in the loop, in ticks: from the render position
   minus the speaker's delay. Negative in a count-in. *started is false
   until the first of the loop reaches the speaker. */
static int32_t heard_position(const looper_state_t *state, bool *started)
{
    double ticks_per_frame = audio_settings.bpm * (double)LOOP_TICKS_PER_BEAT / (60.0 * MUSIC_OUT_RATE);
    int32_t latency = (int32_t)(music_out_latency_frames() * ticks_per_frame);
    int32_t heard_elapsed = (int32_t)state->elapsed - latency;
    *started = heard_elapsed >= 0;
    if (!*started) {
        return state->start;
    }
    int32_t position = state->start + heard_elapsed;
    if (position < 0) {
        return position;
    }
    int32_t length = state->bars * LOOP_TICKS_PER_BAR;
    return position % length;
}

static void refresh_loop(const looper_state_t *state)
{
    row_t row;
    clear_row(&row);
    char line[COLUMNS + 1];
    int32_t length = state->bars * LOOP_TICKS_PER_BAR;
    int progress = 0;
    uint16_t color = state->recording ? CYD_COLOR_RED : COLOR_PLAYING;
    char notes[16];
    if (state->notes == 0) {
        snprintf(notes, sizeof(notes), "EMPTY");
    } else {
        snprintf(notes, sizeof(notes), "%u NOTE%s", state->notes, state->notes == 1 ? "" : "S");
    }
    if (!state->playing) {
        snprintf(line, sizeof(line), " LOOP %u BAR%s  %s", state->bars, state->bars == 1 ? "" : "S",
                 notes);
    } else {
        bool started;
        int32_t heard = heard_position(state, &started);
        if (heard < 0) {
            int beat = (int)((heard + LOOP_TICKS_PER_BAR) / LOOP_TICKS_PER_BEAT) + 1;
            snprintf(line, sizeof(line), " COUNT-IN %d", beat);
            color = cyd_rgb(200, 120, 0);
            progress = (int)((int64_t)(heard + LOOP_TICKS_PER_BAR) * CYD_LCD_WIDTH / LOOP_TICKS_PER_BAR);
        } else {
            int bar = heard / LOOP_TICKS_PER_BAR + 1;
            int beat = heard % LOOP_TICKS_PER_BAR / LOOP_TICKS_PER_BEAT + 1;
            snprintf(line, sizeof(line), " %sBAR %d.%d OF %u  %s", state->recording ? "REC " : "",
                     bar, beat, state->bars, notes);
            progress = (int)((int64_t)heard * CYD_LCD_WIDTH / length);
        }
    }
    bool toasting = (int32_t)(toast_until - xTaskGetTickCount()) > 0;
    if (toasting) {
        snprintf(line, sizeof(line), "%s", toast);
    }
    memcpy(row.text, line, strlen(line));
    for (int column = 0; column < COLUMNS; ++column) {
        row.fg[column] = toasting ? CYD_COLOR_CYAN : state->recording ? CYD_COLOR_RED : CYD_COLOR_YELLOW;
    }
    draw_row(LOOP_ROW, &row);

    if (progress != drawn_progress || color != drawn_progress_color) {
        ESP_ERROR_CHECK(cyd_fill_rect(0, PROGRESS_TOP, CYD_LCD_WIDTH, PROGRESS_HEIGHT,
                                      COLOR_PROGRESS_BACK));
        if (progress > 0) {
            ESP_ERROR_CHECK(cyd_fill_rect(0, PROGRESS_TOP, progress, PROGRESS_HEIGHT, color));
        }
        /* Bar lines. */
        for (int bar = 1; bar < state->bars; ++bar) {
            ESP_ERROR_CHECK(cyd_fill_rect(bar * CYD_LCD_WIDTH / state->bars - 1, PROGRESS_TOP, 2,
                                          PROGRESS_HEIGHT, CYD_COLOR_BLACK));
        }
        drawn_progress = progress;
        drawn_progress_color = color;
    }
}

static uint16_t key_color(int key, uint8_t state)
{
    if (state == KEY_NOT_DRAWN) {
        state = 0; /* a neighbor not drawn yet: it is, right after */
    }
    if (state & KEY_LIVE) {
        return voice_colors[voice_kind];
    }
    if (state & KEY_LOOP) {
        return keys[key].black ? loop_black : loop_white;
    }
    return keys[key].black ? COLOR_BLACK_KEY : COLOR_WHITE_KEY;
}

static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B"};

/* The key's note name: with its octave on a white key ("C4"), without on
   a black one ("C#"), which is too narrow for three letters. */
static void key_label(int key, char *label, size_t size)
{
    int note = base_note + key;
    if (keys[key].black) {
        snprintf(label, size, "%s", note_names[note % 12]);
    } else {
        snprintf(label, size, "%s%d", note_names[note % 12], note / 12 - 1);
    }
}

static uint16_t label_color(int key, uint8_t state)
{
    if (state != KEY_NOT_DRAWN && (state & (KEY_LIVE | KEY_LOOP))) {
        return CYD_COLOR_BLACK;
    }
    if (base_note + key == 60) {
        return CYD_COLOR_RED; /* middle C, to find your place */
    }
    return keys[key].black ? cyd_rgb(170, 170, 170) : cyd_rgb(90, 90, 90);
}

static void draw_label(int key, int bottom)
{
    char label[4];
    key_label(key, label, sizeof(label));
    int width = (int)strlen(label) * CYD_SMALL_FONT_WIDTH;
    uint8_t state = drawn_keys[key];
    ESP_ERROR_CHECK(cyd_draw_small_text(keys[key].x + (keys[key].width - width) / 2,
                                        bottom - CYD_SMALL_FONT_HEIGHT - 8, label,
                                        label_color(key, state), key_color(key, state)));
}

static void draw_black(int key)
{
    ESP_ERROR_CHECK(cyd_fill_rect(keys[key].x, KEYS_TOP, keys[key].width, BLACK_HEIGHT,
                                  key_color(key, drawn_keys[key])));
    draw_label(key, KEYS_TOP + BLACK_HEIGHT);
}

/* A white key, then the black keys beside it, which overlap it. */
static void draw_white(int key)
{
    const piano_key_t *k = &keys[key];
    ESP_ERROR_CHECK(cyd_fill_rect(k->x + 1, KEYS_TOP, k->width - 2, KEYS_HEIGHT,
                                  key_color(key, drawn_keys[key])));
    draw_label(key, CYD_LCD_HEIGHT);
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
        uint8_t state = 0;
        if (live & (1u << key)) {
            state |= KEY_LIVE;
        }
        if (loop_sounding[base_note + key] > 0) {
            state |= KEY_LOOP;
        }
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

static void take_heard_notes(const looper_state_t *state)
{
    int note;
    bool on;
    while (audio_next_heard_note(&note, &on)) {
        if (on) {
            ++loop_sounding[note];
        } else if (loop_sounding[note] > 0) {
            --loop_sounding[note];
        }
    }
    if (!state->playing) {
        memset(loop_sounding, 0, sizeof(loop_sounding));
    }
}

static void full_redraw(void)
{
    memset(drawn_rows, 0, sizeof(drawn_rows));
    memset(drawn_keys, KEY_NOT_DRAWN, sizeof(drawn_keys));
    drawn_progress = -1;
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
}

/* ---- Tempo ---------------------------------------------------------------- */

static void set_bpm(int bpm)
{
    bpm = bpm < MUSIC_TEMPO_MIN ? MUSIC_TEMPO_MIN : bpm > MUSIC_TEMPO_MAX ? MUSIC_TEMPO_MAX : bpm;
    audio_settings.bpm = (uint16_t)bpm;
}

/* Saves the tempo for the other music apps once it has stayed the same
   for a moment, so a run of taps on + is one write. */
static void save_tempo_when_settled(void)
{
    TickType_t now = xTaskGetTickCount();
    if (audio_settings.bpm != changed_bpm) {
        changed_bpm = audio_settings.bpm;
        bpm_changed_at = now;
    }
    if (changed_bpm != saved_bpm && now - bpm_changed_at >= pdMS_TO_TICKS(TEMPO_SAVE_MS)) {
        music_out_save_tempo(changed_bpm);
        saved_bpm = changed_bpm;
    }
}

/* ---- Menu ----------------------------------------------------------------- */

#define MENU_INFO 1
#define MENU_WIFI 2
#define MENU_OUTPUT 3
#define MENU_VOLUME 4
#define MENU_TEMPO 5
#define MENU_LENGTH 6
#define MENU_QUANTIZE 7
#define MENU_COUNT_IN 8
#define MENU_CLEAR 9
#define MENU_LATENCY 10
#define MENU_BACK 11
#define SPLIT_BUTTON_COLUMNS 9
#define VOLUME_STEP 10
#define TEMPO_STEP 10

static char status_text[COLUMNS + 1] = " No speaker: silent";

static void menu_status(const char *status)
{
    snprintf(status_text, sizeof(status_text), "%s", status);
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
}

/* A row with a button at each end and a value between them. */
static void draw_split_row(int index, const char *left, const char *value, const char *right)
{
    row_t row;
    clear_row(&row);
    span_t left_span = {0, SPLIT_BUTTON_COLUMNS};
    span_t right_span = {COLUMNS - SPLIT_BUTTON_COLUMNS, SPLIT_BUTTON_COLUMNS};
    span_t middle = {SPLIT_BUTTON_COLUMNS, COLUMNS - 2 * SPLIT_BUTTON_COLUMNS};
    put_span(&row, left_span, left, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, middle, value, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, right_span, right, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(index, row.text, row.fg, row.bg));
}

static void draw_button_row(int row, const char *label)
{
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, label));
}

static void draw_volume_row(void)
{
    char value[8];
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
    snprintf(value, sizeof(value), "%u BPM", audio_settings.bpm);
    draw_split_row(MENU_TEMPO, "BPM -10", value, "BPM +10");
}

static void draw_length_row(void)
{
    looper_state_t state;
    looper_get_state(&state);
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " LOOP LENGTH  %u BAR%s", state.bars, state.bars == 1 ? "" : "S");
    draw_button_row(MENU_LENGTH, line);
}

static void draw_quantize_row(void)
{
    static const char *const names[QUANTIZE_MODES] = {"OFF", "1/8", "1/16"};
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " QUANTIZE     %s", names[audio_settings.quantize]);
    draw_button_row(MENU_QUANTIZE, line);
}

static void draw_count_in_row(void)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " COUNT-IN     %s", audio_settings.count_in ? "1 BAR" : "OFF");
    draw_button_row(MENU_COUNT_IN, line);
}

static void draw_clear_row(bool armed)
{
    draw_button_row(MENU_CLEAR, armed ? " CLEAR LOOP? TAP AGAIN" : " CLEAR LOOP");
}

static void draw_menu(void)
{
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, " KEYBOARD SETTINGS"));
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
    music_out_draw_wifi_row(MENU_WIFI);
    music_out_draw_output_row(MENU_OUTPUT);
    draw_volume_row();
    draw_tempo_row();
    draw_length_row();
    draw_quantize_row();
    draw_count_in_row();
    draw_clear_row(false);
    music_out_draw_latency_row(MENU_LATENCY);
    draw_button_row(MENU_BACK, " BACK TO KEYS");
}

/* Waits for the audio task to take a command, so the menu shows its
   result: one block is 8 ms. */
static void wait_for_audio(void)
{
    vTaskDelay(pdMS_TO_TICKS(30));
}

static void run_menu(QueueHandle_t touch_queue)
{
    bool clear_armed = false;
    if (speaker_lost_notice) {
        speaker_lost_notice = false;
        snprintf(status_text, sizeof(status_text), " Speaker disconnected");
    }
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
            save_tempo_when_settled();
            looper_state_t state;
            looper_get_state(&state);
            take_heard_notes(&state);
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        bool left = column < SPLIT_BUTTON_COLUMNS;
        bool right = column >= COLUMNS - SPLIT_BUTTON_COLUMNS;
        if (clear_armed && row != MENU_CLEAR) {
            clear_armed = false;
            draw_clear_row(false);
        }
        char status[COLUMNS + 1];
        switch (row) {
        case 0:
        case MENU_BACK:
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
                set_bpm(audio_settings.bpm + (left ? -TEMPO_STEP : TEMPO_STEP));
                draw_tempo_row();
            }
            break;
        case MENU_LENGTH:
            audio_cycle_bars();
            wait_for_audio();
            draw_length_row();
            menu_status(" Longer loops copy the bars");
            break;
        case MENU_QUANTIZE:
            audio_settings.quantize = (quantize_t)((audio_settings.quantize + 1) % QUANTIZE_MODES);
            draw_quantize_row();
            menu_status(" For notes recorded from now");
            break;
        case MENU_COUNT_IN:
            audio_settings.count_in = !audio_settings.count_in;
            draw_count_in_row();
            break;
        case MENU_CLEAR:
            if (clear_armed) {
                audio_clear();
                clear_armed = false;
                draw_clear_row(false);
                menu_status(" Loop cleared");
            } else {
                clear_armed = true;
                draw_clear_row(true);
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

/* ---- Keyboard screen ------------------------------------------------------ */

static void tap_transport(int column, const looper_state_t *state)
{
    if (in_span(PLAY_SPAN, column)) {
        if (state->playing) {
            audio_stop();
        } else {
            audio_play();
        }
    } else if (in_span(REC_SPAN, column)) {
        audio_record(!state->recording);
    } else if (in_span(BPM_DOWN_SPAN, column)) {
        set_bpm(audio_settings.bpm - 1);
    } else if (in_span(BPM_UP_SPAN, column)) {
        set_bpm(audio_settings.bpm + 1);
    } else if (in_span(MET_SPAN, column)) {
        audio_settings.metronome = !audio_settings.metronome;
    }
}

static void show_style(void)
{
    snprintf(toast, sizeof(toast), " %s: %s", synth_style_name(instrument),
             synth_style_about(instrument));
    toast_until = xTaskGetTickCount() + pdMS_TO_TICKS(TOAST_MS);
}

static void tap_sound(int column, const looper_state_t *state)
{
    if (in_span(VOICE_SPAN, column)) {
        voice_kind = (voice_kind + 1) % VOICE_KINDS;
        instrument = synth_instrument(voice_kind, styles[voice_kind]);
        show_style();
    } else if (in_span(STYLE_SPAN, column)) {
        int count = synth_style_count(voice_kind);
        styles[voice_kind] = count > 0 ? (styles[voice_kind] + 1) % count : 0;
        instrument = synth_instrument(voice_kind, styles[voice_kind]);
        show_style();
    } else if (in_span(UNDO_SPAN, column)) {
        if (state->notes > 0) {
            audio_undo();
        }
    }
}

static void tap_octave(int column)
{
    if (in_span(OCTAVE_DOWN_SPAN, column)) {
        if (base_note > LOWEST_BASE) {
            base_note -= 12;
        }
    } else if (in_span(OCTAVE_UP_SPAN, column)) {
        if (base_note < HIGHEST_BASE) {
            base_note += 12;
        }
    } else if (in_span(SUS_SPAN, column)) {
        sustain = !sustain;
        audio_sustain(sustain);
    }
}

/* The keys: fast multi-finger touch, played by on_fingers. */
static void enter_keys(QueueHandle_t touch_queue)
{
    full_redraw();
    xQueueReset(touch_queue);
    keys_live = true;
    cyd_touch_set_fast(true);
}

static void leave_keys(void)
{
    keys_live = false;
    cyd_touch_set_fast(false);
    audio_all_notes_off();
    sustain = false;
}

void ui_run(QueueHandle_t touch_queue)
{
    voice_colors[VOICE_PIANO] = cyd_rgb(255, 150, 30);
    voice_colors[VOICE_ORGAN] = cyd_rgb(240, 70, 60);
    voice_colors[VOICE_STRINGS] = cyd_rgb(70, 210, 100);
    voice_colors[VOICE_SYNTH] = cyd_rgb(220, 90, 230);
    voice_colors[VOICE_PLUCK] = cyd_rgb(240, 210, 40);
    voice_colors[VOICE_BELL] = cyd_rgb(40, 210, 230);
    voice_colors[VOICE_CHIP] = cyd_rgb(110, 140, 255);
    instrument = synth_instrument(voice_kind, styles[voice_kind]);
    loop_white = cyd_rgb(150, 200, 255);
    loop_black = cyd_rgb(50, 110, 200);
    layout_keys();
    saved_bpm = changed_bpm = audio_settings.bpm;

    cyd_touch_set_tap_callback(on_tap, NULL);
    cyd_touch_set_fingers_callback(on_fingers, NULL);
    enter_keys(touch_queue);
    int drawn_base = base_note;
    int drawn_instrument = instrument;
    while (true) {
        if (music_out_check_lost()) {
            speaker_lost_notice = true;
        }
        save_tempo_when_settled();
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
            if (row == 0 && in_span(MENU_SPAN, column)) {
                leave_keys();
                run_menu(touch_queue);
                enter_keys(touch_queue);
                break;
            }
            if (row == 0) {
                tap_transport(column, &state);
            } else if (row == 1) {
                tap_sound(column, &state);
            } else if (row == 2) {
                tap_octave(column);
            }
        }
        /* An octave or voice change recolors or renumbers every key. */
        if (base_note != drawn_base || instrument != drawn_instrument) {
            memset(drawn_keys, KEY_NOT_DRAWN, sizeof(drawn_keys));
            drawn_base = base_note;
            drawn_instrument = instrument;
        }
        looper_get_state(&state);
        take_heard_notes(&state);
        refresh_transport(&state);
        refresh_sound(&state);
        refresh_loop(&state);
        refresh_keys();
    }
}
