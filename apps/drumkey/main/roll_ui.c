/* Drum&Key's Piano Roll: the keyboard loop as a grid of pitch against
   time, to draw, move, resize and delete notes while the loop plays.
   Opened with the drums screen's ROLL button (the screen button goes
   KEYS -> DRUM -> ROLL) or the keys screen's loop row; it works on an
   empty loop too, so notes can be drawn from scratch.

     row 0       PLAY REC - 120 + SNAP KEYS MENU   (the shared header;
                 MENU is the keys' settings menu)
     strip       where the speaker is in the loop
     the grid    14 pitches by up to 2 bars; a key column on the left
     row 11      OCT- OCT+  <  1-2/4  >  DEL  UNDO

   The key column is labelled and plays, while held, in the style the keys
   screen has selected, as new notes do.

   On the grid, with one finger:
     - on an empty spot: a new note on the keys' style, at the snapped
       step (drag right before lifting to make it longer); with notes
       selected, it clears the selection instead
     - on a note: tap to select or deselect it; drag to move it, or the
       whole selection when it is selected (in time and pitch); drag from
       its right end to resize it
   DEL deletes the selected notes.
   The notes are the looper's (looper.h), shared with the keys screen:
   what you record shows here, and what you draw plays there. Each drawn
   note is a take of its own, so UNDO removes the last one. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "dk_audio.h"
#include "dk_ui.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "looper.h"
#include "music_out.h"
#include "synth.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS
#define REFRESH_MS 10

#define STRIP_Y FH
#define STRIP_HEIGHT 8
#define GRID_Y (STRIP_Y + STRIP_HEIGHT + 2)
#define ROW_HEIGHT 27
#define ROWS 14
#define GRID_BOTTOM (GRID_Y + ROWS * ROW_HEIGHT)
#define KEY_WIDTH 60
#define GRID_X KEY_WIDTH
#define GRID_WIDTH (CYD_LCD_WIDTH - GRID_X)
#define TOOL_ROW 11
#define VIEW_BARS_MAX 2

#define LOWEST_NOTE 24  /* C1 */
#define HIGHEST_NOTE 108 /* C8 */
#define DEFAULT_BOTTOM 53 /* F3, so C4 is in the middle */
#define RESIZE_HANDLE 12 /* px at a note's right end that resize it */
#define MOVE_SLOP 8      /* px a finger may drift and still be a tap */
#define AUDITION_MS 180

#define COLOR_WHITE_ROW 0x18E3
#define COLOR_BLACK_ROW 0x0861
#define COLOR_STEP_LINE 0x2945
#define COLOR_BEAT_LINE 0x4A69
#define COLOR_BAR_LINE 0x8410
#define COLOR_STRIP 0x2104
#define COLOR_PLAYING 0x0560
#define COLOR_HEAD CYD_COLOR_WHITE

static const span_t PLAY_SPAN = DK_PLAY_SPAN;
static const span_t REC_SPAN = DK_REC_SPAN;
static const span_t BPM_DOWN_SPAN = DK_BPM_DOWN_SPAN;
static const span_t BPM_SPAN = DK_BPM_SPAN;
static const span_t BPM_UP_SPAN = DK_BPM_UP_SPAN;
static const span_t SNAP_SPAN = DK_SCREEN_SPAN;
static const span_t SWITCH_SPAN = DK_SWITCH_SPAN;
static const span_t MENU_SPAN = DK_MENU_SPAN;

static const span_t OCT_DOWN_SPAN = {0, 4};
static const span_t OCT_UP_SPAN = {4, 4};
static const span_t PAGE_DOWN_SPAN = {8, 2};
static const span_t PAGE_SPAN = {10, 5};
static const span_t PAGE_UP_SPAN = {15, 2};
static const span_t DEL_SPAN = {17, 4};
static const span_t UNDO_SPAN = {22, 4};

/* Snap steps in ticks: a quarter, an eighth, a 16th, a 32nd, or off. */
static const uint32_t snaps[] = {LOOP_TICKS_PER_BEAT, LOOP_TICKS_PER_BEAT / 2,
                                 LOOP_TICKS_PER_BEAT / 4, LOOP_TICKS_PER_BEAT / 8, 0};
static const char *const snap_names[] = {"1/4", "1/8", "1/16", "1/32", "OFF"};
#define SNAPS ((int)(sizeof(snaps) / sizeof(snaps[0])))

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } drag_t;

/* The loop's notes as last read, and as drawn: 12 KB each, in PSRAM. */
static looper_note_t *notes;
static int note_count;
static looper_note_t *drawn_notes;
static int drawn_count = -1;

static int snap_index = 2; /* 1/16 */
static uint32_t last_length = LOOP_TICKS_PER_BEAT / 2;
static int bottom_note = DEFAULT_BOTTOM;
static int first_bar;
static int drawn_bottom = -1;
static int drawn_first_bar = -1;
static int drawn_bars = -1;
static int drawn_head = -2;
static row_t drawn_header;
static row_t drawn_tools;
static bool grid_dirty;
/* The speaker dropped the session: MENU is red until the menu is opened. */
static bool speaker_lost;

/* The selected notes, by start, pitch and style (which identify a note
   to the looper). A selected note that has since gone is just skipped. */
#define MAX_SELECTED 128
static looper_note_t selected[MAX_SELECTED];
static int selected_count;

static bool same_identity(const looper_note_t *a, const looper_note_t *b)
{
    return a->start == b->start && a->note == b->note && a->instrument == b->instrument;
}

static int find_selected(const looper_note_t *note)
{
    for (int i = 0; i < selected_count; ++i) {
        if (same_identity(&selected[i], note)) {
            return i;
        }
    }
    return -1;
}

static void toggle_selected(const looper_note_t *note)
{
    int index = find_selected(note);
    if (index >= 0) {
        selected[index] = selected[--selected_count];
    } else if (selected_count < MAX_SELECTED) {
        selected[selected_count++] = *note;
    }
}

/* One finger editing a note. */
static struct {
    drag_t mode;
    bool group;           /* moving the whole selection */
    looper_note_t origin; /* the grabbed note before the drag */
    looper_note_t originals[MAX_SELECTED]; /* the selection before the drag */
    looper_note_t note;   /* the note as it is now */
    int32_t grab;         /* ticks from the note's start to the finger */
    int press_x, press_y;
    bool moved;
} drag;

/* A key of the key column held down, playing in the keys' style. */
static int key_note = -1;
static int key_instrument;

static TickType_t audition_until;
static int audition_note = -1;
static int audition_instrument;

static bool same_notes(const looper_note_t *a, const looper_note_t *b, int count)
{
    for (int i = 0; i < count; ++i) {
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].note != b[i].note ||
            a[i].instrument != b[i].instrument) {
            return false;
        }
    }
    return true;
}

static uint16_t voice_color(int instrument)
{
    static const uint16_t colors[VOICE_KINDS] = {
        [VOICE_PIANO] = 0xFCA3,   /* orange */
        [VOICE_ORGAN] = 0xF227,   /* red */
        [VOICE_STRINGS] = 0x4E8C, /* green */
        [VOICE_SYNTH] = 0xDAFC,   /* magenta */
        [VOICE_PLUCK] = 0xF685,   /* yellow */
        [VOICE_BELL] = 0x2E9C,    /* cyan */
        [VOICE_CHIP] = 0x6C7F,    /* blue */
    };
    int voice = synth_instrument_voice(instrument);
    return voice >= 0 && voice < VOICE_KINDS ? colors[voice] : CYD_COLOR_WHITE;
}

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

/* ---- Geometry --------------------------------------------------------------- */

static int view_bars(const looper_state_t *state)
{
    return state->bars < VIEW_BARS_MAX ? state->bars : VIEW_BARS_MAX;
}

static uint32_t view_start(void)
{
    return (uint32_t)first_bar * LOOP_TICKS_PER_BAR;
}

static uint32_t view_ticks(const looper_state_t *state)
{
    return (uint32_t)view_bars(state) * LOOP_TICKS_PER_BAR;
}

static int tick_x(const looper_state_t *state, int64_t tick)
{
    return GRID_X + (int)((tick - (int64_t)view_start()) * GRID_WIDTH / view_ticks(state));
}

static int64_t x_tick(const looper_state_t *state, int x)
{
    return (int64_t)view_start() + (int64_t)(x - GRID_X) * view_ticks(state) / GRID_WIDTH;
}

static int row_note(int y)
{
    return bottom_note + (ROWS - 1) - (y - GRID_Y) / ROW_HEIGHT;
}

static int note_y(int note)
{
    return GRID_Y + (ROWS - 1 - (note - bottom_note)) * ROW_HEIGHT;
}

static bool is_black(int note)
{
    int pitch = note % 12;
    return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
}

static uint32_t snap_down(int64_t tick)
{
    uint32_t step = snaps[snap_index];
    if (tick < 0) {
        tick = 0;
    }
    return step > 0 ? (uint32_t)(tick / step * step) : (uint32_t)tick;
}

static uint32_t snap_length(int64_t length)
{
    uint32_t step = snaps[snap_index];
    if (step > 0) {
        length = (length + step / 2) / step * step;
        if (length < step) {
            length = step;
        }
    }
    return length < LOOPER_MIN_LENGTH ? LOOPER_MIN_LENGTH : (uint32_t)length;
}

/* ---- Drawing ------------------------------------------------------------------ */

static void draw_header(const looper_state_t *state)
{
    row_t row;
    clear_row(&row);
    bool playing = dk_is_playing();
    put_span(&row, PLAY_SPAN, playing ? "STOP" : "PLAY", CYD_COLOR_WHITE,
             playing ? COLOR_PLAYING : CYD_COLOR_BUTTON);
    put_span(&row, REC_SPAN, "REC", CYD_COLOR_WHITE,
             state->recording ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    put_span(&row, BPM_DOWN_SPAN, "-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char number[8];
    snprintf(number, sizeof(number), "%u", dk_tempo());
    put_span(&row, BPM_SPAN, number, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, BPM_UP_SPAN, "+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, SNAP_SPAN, snap_names[snap_index], CYD_COLOR_BLACK, CYD_COLOR_CYAN);
    put_span(&row, SWITCH_SPAN, "KEYS", CYD_COLOR_WHITE, DK_KEYS_COLOR);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_header, sizeof(row)) != 0) {
        ESP_ERROR_CHECK(cyd_draw_cells(0, row.text, row.fg, row.bg));
        dk_header_gaps();
        drawn_header = row;
    }
}

static void draw_tools(const looper_state_t *state)
{
    row_t row;
    clear_row(&row);
    bool can_down = bottom_note > LOWEST_NOTE;
    bool can_up = bottom_note + ROWS - 1 < HIGHEST_NOTE;
    put_span(&row, OCT_DOWN_SPAN, "OCT-", can_down ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_down ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, OCT_UP_SPAN, "OCT+", can_up ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_up ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    bool pages = state->bars > VIEW_BARS_MAX;
    put_span(&row, PAGE_DOWN_SPAN, "<", pages ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             pages ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    char page[16];
    if (view_bars(state) > 1) {
        snprintf(page, sizeof(page), "%d-%d/%u", first_bar + 1, first_bar + view_bars(state),
                 state->bars);
    } else {
        snprintf(page, sizeof(page), "%d/%u", first_bar + 1, state->bars);
    }
    put_span(&row, PAGE_SPAN, page, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, PAGE_UP_SPAN, ">", pages ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             pages ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    bool can_delete = selected_count > 0;
    put_span(&row, DEL_SPAN, "DEL", can_delete ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             can_delete ? CYD_COLOR_RED : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, UNDO_SPAN, "UNDO", state->notes > 0 ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
             state->notes > 0 ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    if (memcmp(&row, &drawn_tools, sizeof(row)) != 0) {
        ESP_ERROR_CHECK(cyd_draw_cells(TOOL_ROW, row.text, row.fg, row.bg));
        drawn_tools = row;
    }
}

static void draw_keys_column(void)
{
    for (int i = 0; i < ROWS; ++i) {
        int note = bottom_note + i;
        int y = note_y(note);
        bool black = is_black(note);
        ESP_ERROR_CHECK(cyd_fill_rect(0, y, KEY_WIDTH - 2, ROW_HEIGHT - 1,
                                      black ? CYD_COLOR_BLACK : CYD_COLOR_WHITE));
        ESP_ERROR_CHECK(cyd_fill_rect(0, y + ROW_HEIGHT - 1, KEY_WIDTH - 2, 1, COLOR_STEP_LINE));
        /* Every key's name: C4, C#4, D4 ... */
        static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F",
                                              "F#", "G", "G#", "A", "A#", "B"};
        char name[8];
        snprintf(name, sizeof(name), "%s%d", names[note % 12], note / 12 - 1);
        ESP_ERROR_CHECK(cyd_draw_small_text(4, y + (ROW_HEIGHT - CYD_SMALL_FONT_HEIGHT) / 2, name,
                                            black ? CYD_COLOR_WHITE : CYD_COLOR_BLACK,
                                            black ? CYD_COLOR_BLACK : CYD_COLOR_WHITE));
    }
}

static void draw_grid(const looper_state_t *state)
{
    /* Rows, shaded like the keys. */
    for (int i = 0; i < ROWS; ++i) {
        int note = bottom_note + i;
        ESP_ERROR_CHECK(cyd_fill_rect(GRID_X, note_y(note), GRID_WIDTH, ROW_HEIGHT,
                                      is_black(note) ? COLOR_BLACK_ROW : COLOR_WHITE_ROW));
    }
    /* Step, beat and bar lines. */
    uint32_t sixteenth = LOOP_TICKS_PER_BEAT / 4;
    uint32_t span = view_ticks(state);
    for (uint32_t tick = 0; tick < span; tick += sixteenth) {
        int x = tick_x(state, view_start() + tick);
        bool bar = tick % LOOP_TICKS_PER_BAR == 0;
        bool beat = tick % LOOP_TICKS_PER_BEAT == 0;
        ESP_ERROR_CHECK(cyd_fill_rect(x, GRID_Y, bar ? 2 : 1, ROWS * ROW_HEIGHT,
                                      bar    ? COLOR_BAR_LINE
                                      : beat ? COLOR_BEAT_LINE
                                             : COLOR_STEP_LINE));
    }
    /* Notes in view. */
    int64_t from = view_start();
    int64_t to = from + span;
    for (int i = 0; i < note_count; ++i) {
        const looper_note_t *note = &notes[i];
        int64_t start = note->start;
        int64_t end = start + note->length;
        if (end <= from || start >= to || note->note < bottom_note ||
            note->note >= bottom_note + ROWS) {
            continue;
        }
        int x0 = tick_x(state, start < from ? from : start);
        int x1 = tick_x(state, end > to ? to : end);
        if (x1 - x0 < 4) {
            x1 = x0 + 4;
        }
        int y = note_y(note->note);
        bool held = drag.mode != DRAG_NONE && same_identity(note, &drag.note);
        uint16_t color = held                        ? CYD_COLOR_YELLOW
                         : find_selected(note) >= 0 ? CYD_COLOR_WHITE
                                                    : voice_color(note->instrument);
        ESP_ERROR_CHECK(cyd_fill_rect(x0 + 1, y + 2, x1 - x0 - 2, ROW_HEIGHT - 4, color));
        /* A darker right end, the handle that resizes it. */
        if (x1 - x0 > RESIZE_HANDLE + 4 && start + note->length <= to) {
            ESP_ERROR_CHECK(cyd_fill_rect(x1 - 4, y + 2, 3, ROW_HEIGHT - 4, CYD_COLOR_BLACK));
        }
    }
}

/* Where the speaker is in the loop, in ticks, or -1 before it plays. */
static int32_t heard_tick(const looper_state_t *state)
{
    if (!state->playing) {
        return -1;
    }
    double ticks_per_frame = dk_tempo() * (double)LOOP_TICKS_PER_BEAT / (60.0 * MUSIC_OUT_RATE);
    int32_t latency = (int32_t)(music_out_latency_frames() * ticks_per_frame);
    int32_t position = state->start + ((int32_t)state->elapsed - latency);
    if (position < 0) {
        return -1;
    }
    return position % (state->bars * LOOP_TICKS_PER_BAR);
}

static void draw_strip(const looper_state_t *state)
{
    int32_t heard = heard_tick(state);
    int head = -1;
    if (heard >= 0 && heard >= (int32_t)view_start() &&
        heard < (int32_t)(view_start() + view_ticks(state))) {
        head = tick_x(state, heard);
    }
    if (head == drawn_head) {
        return;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(GRID_X, STRIP_Y, GRID_WIDTH, STRIP_HEIGHT, COLOR_STRIP));
    if (head > GRID_X) { /* nothing to fill when it is at the first step in view */
        ESP_ERROR_CHECK(cyd_fill_rect(GRID_X, STRIP_Y, head - GRID_X, STRIP_HEIGHT,
                                      state->recording ? CYD_COLOR_RED : COLOR_PLAYING));
    }
    if (head >= 0) {
        int marker = head - 1 < GRID_X ? GRID_X : head - 1;
        if (marker > CYD_LCD_WIDTH - 3) {
            marker = CYD_LCD_WIDTH - 3;
        }
        ESP_ERROR_CHECK(cyd_fill_rect(marker, STRIP_Y, 3, STRIP_HEIGHT, COLOR_HEAD));
    }
    drawn_head = head;
}

/* ---- Editing ------------------------------------------------------------------ */

static void audition(int note, int instrument, const looper_state_t *state)
{
    if (state->recording) {
        return; /* it would be recorded as a played note */
    }
    if (audition_note >= 0) {
        keys_note_off(audition_note, audition_instrument);
    }
    keys_note_on(note, instrument);
    audition_note = note;
    audition_instrument = instrument;
    audition_until = xTaskGetTickCount() + pdMS_TO_TICKS(AUDITION_MS);
}

static void end_audition(bool now)
{
    if (audition_note >= 0 && (now || (int32_t)(xTaskGetTickCount() - audition_until) >= 0)) {
        keys_note_off(audition_note, audition_instrument);
        audition_note = -1;
    }
}

/* The note under a finger, if any, and whether the finger is on its end. */
static int note_at(const looper_state_t *state, int x, int y, bool *on_end)
{
    int pitch = row_note(y);
    int64_t tick = x_tick(state, x);
    for (int i = note_count - 1; i >= 0; --i) {
        const looper_note_t *note = &notes[i];
        if (note->note != pitch || tick < note->start || tick >= note->start + note->length) {
            continue;
        }
        int end_x = tick_x(state, (int64_t)note->start + note->length);
        *on_end = end_x - x <= RESIZE_HANDLE;
        return i;
    }
    return -1;
}

static void press_grid(const looper_state_t *state, int x, int y)
{
    bool on_end = false;
    int index = note_at(state, x, y, &on_end);
    drag.press_x = x;
    drag.press_y = y;
    drag.moved = false;
    if (index >= 0) {
        drag.note = notes[index];
        drag.origin = notes[index];
        drag.mode = on_end ? DRAG_RESIZE : DRAG_MOVE;
        drag.grab = (int32_t)(x_tick(state, x) - notes[index].start);
        drag.group = drag.mode == DRAG_MOVE && find_selected(&drag.note) >= 0 && selected_count > 1;
        memcpy(drag.originals, selected, sizeof(selected[0]) * (size_t)selected_count);
        audition(drag.note.note, drag.note.instrument, state);
    } else if (selected_count > 0) {
        selected_count = 0; /* a tap off the notes clears the selection */
    } else {
        looper_note_t note = {
            .start = snap_down(x_tick(state, x)),
            .length = last_length,
            .note = (uint8_t)row_note(y),
            .instrument = (uint8_t)keys_current_instrument(),
        };
        if (!looper_add_note(&note)) {
            return; /* the loop is full */
        }
        drag.note = note;
        drag.mode = DRAG_RESIZE; /* drag right to draw it longer */
        drag.moved = true;       /* a new note isn't deleted on lift */
        audition(note.note, note.instrument, state);
    }
    grid_dirty = true;
}

/* Moves the whole selection by the grabbed note's change. */
static void move_group(const looper_note_t *grabbed_to, const looper_state_t *state)
{
    int32_t delta_ticks = (int32_t)grabbed_to->start - (int32_t)drag.origin.start;
    int delta_pitch = grabbed_to->note - drag.origin.note;
    uint32_t loop_ticks = (uint32_t)state->bars * LOOP_TICKS_PER_BAR;
    for (int i = 0; i < selected_count; ++i) {
        looper_note_t to = drag.originals[i];
        int64_t start = (int64_t)to.start + delta_ticks;
        start = start < 0 ? 0 : start;
        if (start + LOOPER_MIN_LENGTH > loop_ticks) {
            start = loop_ticks - LOOPER_MIN_LENGTH;
        }
        int pitch = to.note + delta_pitch;
        to.start = (uint32_t)start;
        to.note = (uint8_t)(pitch < 0 ? 0 : pitch > 127 ? 127 : pitch);
        if (!same_notes(&to, &selected[i], 1) && looper_replace_note(&selected[i], &to)) {
            selected[i] = to;
        }
    }
}

/* Follows the editing finger; on lift, a note tapped without moving is
   selected or deselected. */
static void follow_drag(const looper_state_t *state)
{
    cyd_touch_point_t held[5];
    int count = cyd_touch_held(held, 5);
    const cyd_touch_point_t *finger = NULL;
    for (int i = 0; i < count; ++i) {
        if (held[i].y >= GRID_Y - ROW_HEIGHT && held[i].y < GRID_BOTTOM + ROW_HEIGHT) {
            finger = &held[i];
            break;
        }
    }
    if (finger == NULL) {
        if (drag.mode == DRAG_MOVE && !drag.moved) {
            toggle_selected(&drag.note);
        }
        drag.mode = DRAG_NONE;
        end_audition(true);
        grid_dirty = true;
        return;
    }
    if (abs(finger->x - drag.press_x) > MOVE_SLOP || abs(finger->y - drag.press_y) > MOVE_SLOP) {
        drag.moved = true;
    }
    if (!drag.moved) {
        return;
    }
    looper_note_t to = drag.note;
    int y = finger->y < GRID_Y ? GRID_Y : finger->y >= GRID_BOTTOM ? GRID_BOTTOM - 1 : finger->y;
    if (drag.mode == DRAG_MOVE) {
        to.start = snap_down(x_tick(state, finger->x) - drag.grab);
        to.note = (uint8_t)row_note(y);
    } else {
        to.length = snap_length(x_tick(state, finger->x) - drag.note.start);
        last_length = to.length;
    }
    uint32_t loop_ticks = (uint32_t)state->bars * LOOP_TICKS_PER_BAR;
    if (to.start + LOOPER_MIN_LENGTH > loop_ticks) {
        to.start = loop_ticks - LOOPER_MIN_LENGTH;
    }
    if (same_notes(&to, &drag.note, 1)) {
        return;
    }
    if (drag.group) {
        move_group(&to, state);
    } else if (looper_replace_note(&drag.note, &to)) {
        int index = find_selected(&drag.note);
        if (index >= 0) {
            selected[index] = to; /* it keeps its selection */
        }
    } else {
        return;
    }
    if (to.note != drag.note.note) {
        audition(to.note, to.instrument, state);
    }
    drag.note = to;
    grid_dirty = true;
}

/* The key column is a keyboard in the keys screen's style: a key plays
   while it is held (and is recorded while REC is on, as on the keys). */
static void press_key(int note)
{
    if (key_note >= 0) {
        keys_note_off(key_note, key_instrument);
    }
    key_note = note;
    key_instrument = keys_current_instrument();
    keys_note_on(key_note, key_instrument);
}

static void follow_key(void)
{
    if (key_note < 0) {
        return;
    }
    cyd_touch_point_t held[5];
    int count = cyd_touch_held(held, 5);
    for (int i = 0; i < count; ++i) {
        if (held[i].x < GRID_X + MOVE_SLOP && held[i].y >= GRID_Y && held[i].y < GRID_BOTTOM) {
            int note = row_note(held[i].y);
            if (note != key_note) {
                press_key(note); /* slid to another key */
            }
            return;
        }
    }
    keys_note_off(key_note, key_instrument);
    key_note = -1;
}

static void tap_header(int column, const looper_state_t *state)
{
    if (in_span(PLAY_SPAN, column)) {
        dk_play(!dk_is_playing());
    } else if (in_span(REC_SPAN, column)) {
        /* As on the keys screen: from stop, both loops start. */
        if (!state->recording && !dk_is_playing()) {
            dk_play(true);
        }
        keys_record(!state->recording);
    } else if (in_span(BPM_DOWN_SPAN, column)) {
        dk_set_tempo(dk_tempo() - 1);
    } else if (in_span(BPM_UP_SPAN, column)) {
        dk_set_tempo(dk_tempo() + 1);
    } else if (in_span(SNAP_SPAN, column)) {
        snap_index = (snap_index + 1) % SNAPS;
    }
}

static void tap_tools(int column, const looper_state_t *state)
{
    if (in_span(OCT_DOWN_SPAN, column)) {
        bottom_note = bottom_note - 12 < LOWEST_NOTE ? LOWEST_NOTE : bottom_note - 12;
    } else if (in_span(OCT_UP_SPAN, column)) {
        int top = HIGHEST_NOTE - (ROWS - 1);
        bottom_note = bottom_note + 12 > top ? top : bottom_note + 12;
    } else if (state->bars > VIEW_BARS_MAX && in_span(PAGE_DOWN_SPAN, column)) {
        first_bar = first_bar - VIEW_BARS_MAX < 0 ? state->bars - VIEW_BARS_MAX
                                                  : first_bar - VIEW_BARS_MAX;
    } else if (state->bars > VIEW_BARS_MAX && in_span(PAGE_UP_SPAN, column)) {
        first_bar = first_bar + VIEW_BARS_MAX >= state->bars ? 0 : first_bar + VIEW_BARS_MAX;
    } else if (in_span(DEL_SPAN, column) && selected_count > 0) {
        for (int i = 0; i < selected_count; ++i) {
            looper_remove_note(&selected[i]);
        }
        selected_count = 0;
        grid_dirty = true;
    } else if (in_span(UNDO_SPAN, column) && state->notes > 0) {
        keys_undo();
    }
}

/* ---- The screen ----------------------------------------------------------------- */

static void full_redraw(void)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    memset(&drawn_header, 0, sizeof(drawn_header));
    memset(&drawn_tools, 0, sizeof(drawn_tools));
    drawn_bottom = -1;
    drawn_first_bar = -1;
    drawn_bars = -1;
    drawn_head = -2;
    drawn_count = -1;
    grid_dirty = true;
}

/* Runs the Piano Roll until KEYS is tapped. */
void roll_screen(QueueHandle_t touch_queue)
{
    if (notes == NULL) {
        size_t size = sizeof(looper_note_t) * LOOP_MAX_NOTES;
        notes = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        drawn_notes = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (notes == NULL || drawn_notes == NULL) {
            free(notes);
            free(drawn_notes);
            notes = drawn_notes = NULL;
            dk_next_screen = DK_KEYS;
            return;
        }
    }
    full_redraw();
    xQueueReset(touch_queue);
    cyd_touch_set_tap_callback(NULL, NULL);
    cyd_touch_set_fingers_callback(NULL, NULL);
    cyd_touch_set_fast(true); /* drags follow every 5 ms */
    drag.mode = DRAG_NONE;
    while (true) {
        if (music_out_check_lost()) {
            speaker_lost = true;
        }
        looper_state_t state;
        looper_get_state(&state);
        if (first_bar >= state.bars || first_bar % VIEW_BARS_MAX != 0) {
            first_bar = 0;
        }
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        while (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            int row = point.y / FH;
            int column = point.x / FW;
            if (row == 0) {
                if (in_span(MENU_SPAN, column)) {
                    /* The keys' menu: loop length, quantize, clear, speaker. */
                    end_audition(true);
                    if (key_note >= 0) {
                        keys_note_off(key_note, key_instrument);
                        key_note = -1;
                    }
                    drag.mode = DRAG_NONE;
                    speaker_lost = false;
                    cyd_touch_set_fast(false);
                    keys_menu(touch_queue);
                    cyd_touch_set_fast(true);
                    full_redraw();
                    xQueueReset(touch_queue);
                    break;
                }
                if (in_span(SWITCH_SPAN, column)) {
                    end_audition(true);
                    if (key_note >= 0) {
                        keys_note_off(key_note, key_instrument);
                        key_note = -1;
                    }
                    cyd_touch_set_fast(false);
                    return; /* on to the keys */
                }
                tap_header(column, &state);
            } else if (row == TOOL_ROW) {
                tap_tools(column, &state);
            } else if (point.y >= GRID_Y && point.y < GRID_BOTTOM && point.x < GRID_X) {
                press_key(row_note(point.y));
            } else if (point.y >= GRID_Y && point.y < GRID_BOTTOM && point.x >= GRID_X &&
                       drag.mode == DRAG_NONE) {
                looper_get_state(&state);
                note_count = looper_get_notes(notes, LOOP_MAX_NOTES);
                press_grid(&state, point.x, point.y);
            }
        }
        if (drag.mode != DRAG_NONE) {
            follow_drag(&state);
        }
        follow_key();
        end_audition(false);

        /* The loop's notes change from here, from recording and from UNDO. */
        note_count = looper_get_notes(notes, LOOP_MAX_NOTES);
        if (note_count != drawn_count || !same_notes(notes, drawn_notes, note_count)) {
            grid_dirty = true;
        }
        if (bottom_note != drawn_bottom) {
            draw_keys_column();
            drawn_bottom = bottom_note;
            grid_dirty = true;
        }
        if (first_bar != drawn_first_bar || state.bars != drawn_bars) {
            drawn_first_bar = first_bar;
            drawn_bars = state.bars;
            grid_dirty = true;
        }
        if (grid_dirty) {
            draw_grid(&state);
            memcpy(drawn_notes, notes, sizeof(notes[0]) * (size_t)note_count);
            drawn_count = note_count;
            grid_dirty = false;
        }
        draw_header(&state);
        draw_tools(&state);
        draw_strip(&state);
        /* The keys screen lights keys from these; drop them while away. */
        int heard_note;
        bool heard_on;
        while (keys_next_heard_note(&heard_note, &heard_on)) {
        }
        if (wait == 0) {
            vTaskDelay(1); /* taps came in: still let the idle task run */
        }
    }
}
