/* LiveDrum's screens, 800x480.

   Pads screen:
     row 0        PLAY REC - 120 + EDIT BANK MENU   (BANK is the screen
                  switch in Drum&Key)
     6 px         the loop's beats, lit as the speaker plays them
     rows 1-10    8, 12 or 16 pads, four across; pad 1 bottom left
     row 11       2BAR Q16 MET UNDO CLR  1.3 24

   A pad plays on touch-down, in the touch task (on_tap), so a hit reaches
   the audio task without waiting for this one to draw; each finger is its
   own hit. EDIT, then a pad, opens that pad's sound picker; so does
   holding a pad while the loop is stopped. */

#include "ld_ui.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "kit.h"
#include "ld_engine.h"
#include "ld_sounds.h"
#include "music_out.h"
#include "nvs.h"
#include "sdcard.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

_Static_assert(CYD_LCD_WIDTH == 800 && CYD_TEXT_ROWS == 12,
               "LiveDrum is laid out for the 800x480 Waveshare S3 board");

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS
#define SW CYD_SMALL_FONT_WIDTH
#define SH CYD_SMALL_FONT_HEIGHT

#define STRIP_ROW (CYD_TEXT_ROWS - 1)
#define PROGRESS_TOP FH
#define PROGRESS_HEIGHT 6
#define PAD_TOP (FH + PROGRESS_HEIGHT)
#define PAD_BOTTOM (STRIP_ROW * FH)
#define PAD_COLUMNS 4
#define PAD_WIDTH (CYD_LCD_WIDTH / PAD_COLUMNS)
#define PAD_GAP 8

#define REFRESH_MS 10
#define FLASH_MS 110
#define HOLD_MS 600
#define ARM_MS 3000
#define TEMPO_SAVE_MS 2000
#define NO_BEAT INT_MIN
#define NOT_DRAWN 0xFFFFFFFFu

#define COLOR_PLAYING 0x0560
#define COLOR_EMPTY 0x2104
#define COLOR_WAV 0x05B5   /* teal */
#define COLOR_SYNTH 0xFB76 /* pink */
#define COLOR_BEAT 0x39C7
#define COLOR_BAR 0x7BEF

#define PREFS_NAMESPACE "livedrum"

typedef struct {
    int column;
    int width;
} span_t;

static const span_t PLAY_SPAN = {0, 4};
static const span_t REC_SPAN = {4, 3};
static const span_t BPM_DOWN_SPAN = {7, 2};
static const span_t BPM_SPAN = {9, 3};
static const span_t BPM_UP_SPAN = {12, 2};
static const span_t EDIT_SPAN = {14, 4};
static const span_t SWITCH_SPAN = {18, 4};
static const span_t MENU_SPAN = {22, 4};
static const int HEADER_GAPS[] = {4, 7, 14, 18, 22};

static const span_t BARS_SPAN = {0, 4};
static const span_t QUANT_SPAN = {4, 4};
static const span_t MET_SPAN = {8, 3};
static const span_t UNDO_SPAN = {11, 4};
static const span_t CLEAR_SPAN = {15, 4};
static const span_t POS_SPAN = {19, 7};
static const int STRIP_GAPS[] = {4, 8, 11, 15, 19};

typedef struct {
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
} row_t;

static const ld_host_t *host;
static uint16_t track_colors[DRUM_TRACKS];

/* Settings kept in NVS. */
static int layout = 12; /* pads on screen: 8, 12 or 16 */
static bool count_in = true;

/* Pads screen state. pads_live, edit_mode and the tap fields are shared
   with the touch task (on_tap). */
static volatile bool pads_live;
static volatile bool edit_mode;
static atomic_uint live_hits;
static volatile int tap_pad = -1;
static volatile TickType_t tap_at;
static atomic_uint tap_count;
static TickType_t flash_until[LD_PADS];
static bool flash_live[LD_PADS];
static int heard_beat = NO_BEAT;
static bool clear_armed;
static TickType_t clear_armed_at;
static bool speaker_lost_notice;
static char own_status[COLUMNS + 1] = " No speaker: silent";
/* The menus' status line: the host's, shared with its other menus, or ours. */
static char *status_text = own_status;
#define STATUS_SIZE (COLUMNS + 1)

static row_t drawn_header;
static row_t drawn_strip;
static uint32_t drawn_pads[LD_PADS];
static int drawn_beat;
static int drawn_beats;

/* ---- Helpers -------------------------------------------------------------- */

static uint16_t dim(uint16_t color)
{
    return (uint16_t)((color >> 1) & 0x7BEF);
}

/* Halfway to white. */
static uint16_t lighten(uint16_t color)
{
    return (uint16_t)(((color >> 1) & 0x7BEF) + 0x7BEF);
}

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

static void row_init(row_t *row, uint16_t fg, uint16_t bg)
{
    memset(row->text, ' ', COLUMNS);
    row->text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        row->fg[column] = fg;
        row->bg[column] = bg;
    }
}

/* Writes label into a span of the row, centered, or from its left edge. */
static void put(row_t *row, span_t span, const char *label, uint16_t fg, uint16_t bg, bool center)
{
    int length = (int)strlen(label);
    if (length > span.width) {
        length = span.width;
    }
    int start = span.column + (center ? (span.width - length) / 2 : 0);
    for (int column = span.column; column < span.column + span.width; ++column) {
        row->fg[column] = fg;
        row->bg[column] = bg;
    }
    memcpy(row->text + start, label, (size_t)length);
}

static void put_span(row_t *row, span_t span, const char *label, uint16_t fg, uint16_t bg)
{
    put(row, span, label, fg, bg, true);
}

static void put_text(row_t *row, span_t span, const char *label, uint16_t fg, uint16_t bg)
{
    put(row, span, label, fg, bg, false);
}

static void draw_row(int row, const row_t *cells)
{
    ESP_ERROR_CHECK(cyd_draw_cells(row, cells->text, cells->fg, cells->bg));
}

/* Thin black gaps between buttons that sit side by side. */
static void draw_gaps(int row, const int *columns, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK(cyd_fill_rect(columns[i] * FW - 1, row * FH, 2, FH, CYD_COLOR_BLACK));
    }
}

static void draw_text_row(int row, uint16_t fg, uint16_t bg, const char *text)
{
    ESP_ERROR_CHECK(cyd_text_row(row, fg, bg, text));
}

/* A row with a button at each end and a value between them. */
static void draw_split_row(int row, const char *left, const char *value, const char *right)
{
    row_t cells;
    row_init(&cells, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    put_span(&cells, (span_t){0, 6}, left, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&cells, (span_t){6, 14}, value, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&cells, (span_t){20, 6}, right, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(row, &cells);
}

/* A title across the row with a BACK button on the right. */
static void draw_title_row(const char *title)
{
    row_t cells;
    row_init(&cells, CYD_COLOR_BLACK, CYD_COLOR_YELLOW);
    put_text(&cells, (span_t){0, 22}, title, CYD_COLOR_BLACK, CYD_COLOR_YELLOW);
    put_span(&cells, (span_t){22, 4}, "BACK", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(0, &cells);
}

static void set_status(const char *status)
{
    snprintf(status_text, STATUS_SIZE, "%s", status);
    draw_text_row(1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text);
}

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

/* ---- Settings ---------------------------------------------------------------- */

static void load_prefs(void)
{
    nvs_handle_t handle;
    if (nvs_open(PREFS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t value;
    if (nvs_get_u8(handle, "layout", &value) == ESP_OK && (value == 8 || value == 12 || value == 16)) {
        layout = value;
    }
    if (nvs_get_u8(handle, "count_in", &value) == ESP_OK) {
        count_in = value != 0;
    }
    nvs_close(handle);
}

static void save_prefs(void)
{
    nvs_handle_t handle;
    if (nvs_open(PREFS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, "layout", (uint8_t)layout);
    nvs_set_u8(handle, "count_in", count_in);
    nvs_commit(handle);
    nvs_close(handle);
}

static uint16_t saved_bpm;
static uint16_t changed_bpm;
static TickType_t bpm_changed_at;

/* Saves the tempo for the other music apps once it has settled, so a run
   of taps on + is one write. */
static void save_tempo_when_settled(void)
{
    TickType_t now = xTaskGetTickCount();
    uint16_t bpm = host->tempo();
    if (bpm != changed_bpm) {
        changed_bpm = bpm;
        bpm_changed_at = now;
    }
    if (changed_bpm != saved_bpm && now - bpm_changed_at >= pdMS_TO_TICKS(TEMPO_SAVE_MS)) {
        music_out_save_tempo(changed_bpm);
        saved_bpm = changed_bpm;
    }
}

/* ---- Pads ------------------------------------------------------------------- */

static int pad_rows(void)
{
    return layout / PAD_COLUMNS;
}

static int pad_height(void)
{
    return (PAD_BOTTOM - PAD_TOP) / pad_rows();
}

/* Pad 1 is bottom left, as on a drum machine, so more pads add rows on top. */
static void pad_rect(int pad, int *x, int *y, int *width, int *height)
{
    int column = pad % PAD_COLUMNS;
    int row = pad_rows() - 1 - pad / PAD_COLUMNS;
    *x = column * PAD_WIDTH + PAD_GAP / 2;
    *y = PAD_TOP + row * pad_height() + PAD_GAP / 2;
    *width = PAD_WIDTH - PAD_GAP;
    *height = pad_height() - PAD_GAP;
}

/* The pad under a point, gaps included, or -1. */
static int pad_at(int x, int y)
{
    if (y < PAD_TOP || y >= PAD_BOTTOM) {
        return -1;
    }
    int column = clamp(x / PAD_WIDTH, 0, PAD_COLUMNS - 1);
    int row = clamp((y - PAD_TOP) / pad_height(), 0, pad_rows() - 1);
    return (pad_rows() - 1 - row) * PAD_COLUMNS + column;
}

static uint16_t pad_color(int pad)
{
    const ld_source_t *source = ld_sounds_source(pad);
    switch (source->kind) {
    case LD_SOURCE_KIT:
        return track_colors[source->track % DRUM_TRACKS];
    case LD_SOURCE_WAV:
        return COLOR_WAV;
    case LD_SOURCE_SYNTH:
        return COLOR_SYNTH;
    default:
        return COLOR_EMPTY;
    }
}

/* "L60 +3 C1": the pad's level, pitch and choke group where not default. */
static void pad_info(int pad, char *info, size_t size)
{
    const ld_pad_settings_t *settings = ld_pad(pad);
    int used = 0;
    info[0] = '\0';
    if (settings->level != LD_LEVEL_DEFAULT) {
        used += snprintf(info + used, size - used, "L%u ", settings->level);
    }
    if (settings->pitch != 0 && used < (int)size) {
        used += snprintf(info + used, size - used, "%+d ", settings->pitch);
    }
    if (settings->choke != 0 && used < (int)size) {
        snprintf(info + used, size - used, "C%u", settings->choke);
    }
}

static void draw_small(int x, int y, int max_width, const char *text, uint16_t fg, uint16_t bg)
{
    char clipped[LD_PATH_MAX];
    int fit = max_width / SW;
    snprintf(clipped, sizeof(clipped), "%.*s", fit, text);
    if (clipped[0] != '\0') {
        ESP_ERROR_CHECK(cyd_draw_small_text(x, y, clipped, fg, bg));
    }
}

static void draw_pad(int pad, uint16_t fill, uint16_t text)
{
    int x, y, width, height;
    pad_rect(pad, &x, &y, &width, &height);
    ESP_ERROR_CHECK(cyd_fill_rect(x, y, width, height, fill));
    char number[4];
    snprintf(number, sizeof(number), "%d", pad + 1);
    draw_small(x + 6, y + 4, width - 12, number, text, fill);
    const char *name = ld_sounds_name(pad);
    int name_width = (int)strlen(name) * SW;
    if (name_width > width - 12) {
        name_width = width - 12;
    }
    draw_small(x + (width - name_width) / 2, y + (height - SH) / 2, width - 12, name, text, fill);
    char info[16];
    pad_info(pad, info, sizeof(info));
    draw_small(x + 6, y + height - SH - 4, width - 12, info, text, fill);
}

/* What a pad looks like now: its fill, and bit 16 for white text. */
static uint32_t pad_look(int pad, TickType_t now)
{
    uint16_t color = pad_color(pad);
    bool flashing = (int32_t)(flash_until[pad] - now) > 0;
    uint16_t fill = flashing ? (flash_live[pad] ? CYD_COLOR_WHITE : lighten(color))
                    : edit_mode ? dim(color)
                                : color;
    bool white_text = !flashing && (edit_mode || pad_color(pad) == COLOR_EMPTY);
    return fill | (white_text ? 0x10000u : 0);
}

static void refresh_pads(void)
{
    TickType_t now = xTaskGetTickCount();
    for (int pad = 0; pad < layout; ++pad) {
        uint32_t look = pad_look(pad, now);
        if (look != drawn_pads[pad]) {
            draw_pad(pad, (uint16_t)look, (look & 0x10000u) ? CYD_COLOR_WHITE : CYD_COLOR_BLACK);
            drawn_pads[pad] = look;
        }
    }
}

/* Runs in the touch task for every tap: a pad goes to the audio task at
   once instead of waiting for this task to draw. In EDIT mode the tap is
   queued instead, for the UI to open the pad. */
static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    if (!pads_live || edit_mode) {
        return false;
    }
    int pad = pad_at(point->x, point->y);
    if (pad < 0 || pad >= layout) {
        return false;
    }
    ld_hit(pad);
    atomic_fetch_or(&live_hits, 1u << pad);
    tap_pad = pad;
    tap_at = xTaskGetTickCount();
    atomic_fetch_add(&tap_count, 1);
    return true;
}

static void flash(int pad, bool live)
{
    flash_until[pad] = xTaskGetTickCount() + pdMS_TO_TICKS(FLASH_MS);
    flash_live[pad] = live;
}

/* Lights the pads just played, and those the loop has played by now, and
   follows the beat. */
static void update_flashes(void)
{
    unsigned hit = atomic_exchange(&live_hits, 0);
    for (int pad = 0; pad < LD_PADS; ++pad) {
        if (hit & (1u << pad)) {
            flash(pad, true);
        }
    }
    ld_heard_t heard;
    while (ld_next_heard(&heard)) {
        if (heard.type == LD_HEARD_PAD) {
            flash(heard.pad, false);
        } else {
            heard_beat = heard.beat;
        }
    }
    if (!host->is_playing()) {
        heard_beat = NO_BEAT;
    }
}

/* ---- Header, strip and beat line ------------------------------------------- */

static void refresh_header(void)
{
    row_t row;
    row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    bool playing = host->is_playing();
    put_span(&row, PLAY_SPAN, playing ? "STOP" : "PLAY", CYD_COLOR_WHITE,
             playing ? COLOR_PLAYING : CYD_COLOR_BUTTON);
    put_span(&row, REC_SPAN, "REC", CYD_COLOR_WHITE,
             ld_is_recording() ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    put_span(&row, BPM_DOWN_SPAN, "-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char bpm[8];
    snprintf(bpm, sizeof(bpm), "%u", host->tempo());
    put_span(&row, BPM_SPAN, bpm, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, BPM_UP_SPAN, "+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, EDIT_SPAN, "EDIT", edit_mode ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             edit_mode ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON);
    if (host->switch_label != NULL) {
        put_span(&row, SWITCH_SPAN, host->switch_label, CYD_COLOR_WHITE, host->switch_color);
    } else {
        put_span(&row, SWITCH_SPAN, "BANK", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    }
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    if (memcmp(&row, &drawn_header, sizeof(row)) != 0) {
        draw_row(0, &row);
        draw_gaps(0, HEADER_GAPS, sizeof(HEADER_GAPS) / sizeof(HEADER_GAPS[0]));
        drawn_header = row;
    }
}

static const char *quantize_label(int quantize)
{
    static const char *const labels[LD_QUANTIZE_MODES] = {"QOFF", "Q8", "Q16", "Q32", "Q8T", "Q16T"};
    return labels[clamp(quantize, 0, LD_QUANTIZE_MODES - 1)];
}

static void refresh_strip(void)
{
    if (clear_armed && xTaskGetTickCount() - clear_armed_at > pdMS_TO_TICKS(ARM_MS)) {
        clear_armed = false;
    }
    row_t row;
    row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    char bars[8];
    snprintf(bars, sizeof(bars), "%dBAR", ld_bars());
    put_span(&row, BARS_SPAN, bars, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, QUANT_SPAN, quantize_label(ld_quantize), CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, MET_SPAN, "MET", ld_metronome ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             ld_metronome ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    put_span(&row, UNDO_SPAN, "UNDO", CYD_COLOR_WHITE,
             ld_take_count() > 0 ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED);
    put_span(&row, CLEAR_SPAN, clear_armed ? "CLR?" : "CLR", CYD_COLOR_WHITE,
             clear_armed           ? CYD_COLOR_RED
             : ld_hit_count() > 0 ? CYD_COLOR_BUTTON
                                  : CYD_COLOR_BUTTON_DISABLED);
    char position[12];
    if (heard_beat == NO_BEAT) {
        snprintf(position, sizeof(position), ld_hit_count() < 100 ? "%d HITS" : "%d H", ld_hit_count());
    } else if (heard_beat < 0) {
        snprintf(position, sizeof(position), "IN %d", -heard_beat);
    } else {
        snprintf(position, sizeof(position), "%d.%d %d", heard_beat / LD_BEATS_PER_BAR + 1,
                 heard_beat % LD_BEATS_PER_BAR + 1, ld_hit_count());
    }
    put_span(&row, POS_SPAN, position, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    if (memcmp(&row, &drawn_strip, sizeof(row)) != 0) {
        draw_row(STRIP_ROW, &row);
        draw_gaps(STRIP_ROW, STRIP_GAPS, sizeof(STRIP_GAPS) / sizeof(STRIP_GAPS[0]));
        drawn_strip = row;
    }
}

/* The beat line under the header: a segment per beat of the loop, the one
   heard now lit (red in the count-in). */
static void refresh_beats(void)
{
    int beats = ld_bars() * LD_BEATS_PER_BAR;
    if (beats == drawn_beats && heard_beat == drawn_beat) {
        return;
    }
    ESP_ERROR_CHECK(cyd_fill_rect(0, PROGRESS_TOP, CYD_LCD_WIDTH, PROGRESS_HEIGHT, CYD_COLOR_BLACK));
    for (int beat = 0; beat < beats; ++beat) {
        int x0 = beat * CYD_LCD_WIDTH / beats;
        int x1 = (beat + 1) * CYD_LCD_WIDTH / beats;
        uint16_t color = beat == heard_beat           ? CYD_COLOR_WHITE
                         : beat % LD_BEATS_PER_BAR == 0 ? COLOR_BAR
                                                        : COLOR_BEAT;
        ESP_ERROR_CHECK(cyd_fill_rect(x0 + 1, PROGRESS_TOP + 1, x1 - x0 - 2, PROGRESS_HEIGHT - 2, color));
    }
    if (heard_beat < 0 && heard_beat != NO_BEAT) {
        ESP_ERROR_CHECK(cyd_fill_rect(0, PROGRESS_TOP + 1, CYD_LCD_WIDTH, PROGRESS_HEIGHT - 2, CYD_COLOR_RED));
    }
    drawn_beats = beats;
    drawn_beat = heard_beat;
}

static void invalidate(void)
{
    memset(&drawn_header, 0, sizeof(drawn_header));
    memset(&drawn_strip, 0, sizeof(drawn_strip));
    for (int pad = 0; pad < LD_PADS; ++pad) {
        drawn_pads[pad] = NOT_DRAWN;
    }
    drawn_beats = -1;
    drawn_beat = NO_BEAT;
}

static void refresh(void)
{
    refresh_header();
    refresh_beats();
    refresh_pads();
    refresh_strip();
}

static void full_redraw(void)
{
    invalidate();
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    refresh();
}

/* ---- Pad editor: the sound picker, level, pitch and choke ------------------ */

#define EDIT_TABS 2
#define EDIT_CONTENT 3
#define EDIT_CONTENT_ROWS 6
#define EDIT_LEVEL 9
#define EDIT_PITCH 10
#define EDIT_NAV 11
#define CARD_LIST 4 /* the folder's path is on EDIT_CONTENT */
#define CARD_LIST_ROWS 5
#define CARD_ENTRIES 160
#define LEVEL_STEP 5

typedef enum {
    TAB_KITS,
    TAB_CARD,
    TAB_SYNTH,
} edit_tab_t;

static const span_t KITS_TAB = {0, 6};
static const span_t CARD_TAB = {6, 8};
static const span_t SYNTH_TAB = {14, 6};
static const span_t OFF_TAB = {20, 6};
static const span_t KIT_NAME_SPAN = {0, 6};
static const span_t SCROLL_SPAN = {22, 4};
static const span_t CARD_PATH_SPAN = {0, 18};
static const span_t CARD_UP_SPAN = {18, 4};
static const span_t CARD_ENTRY_SPAN = {0, 22};
static const span_t NAV_PREV = {0, 5};
static const span_t NAV_PLAY = {5, 7};
static const span_t NAV_CHOKE = {12, 9};
static const span_t NAV_NEXT = {21, 5};

static int edit_pad;
static edit_tab_t edit_tab = TAB_KITS;
static int kit_scroll;
static char card_folder[LD_PATH_MAX];
static ld_entry_t *card_entries;
static int card_count;
static int card_scroll;
static bool card_listed;

static span_t kit_cell(int track)
{
    return (span_t){KIT_NAME_SPAN.width + 2 * track, 2};
}

static void draw_edit_title(void)
{
    char title[COLUMNS + 1];
    snprintf(title, sizeof(title), " PAD %d: %s", edit_pad + 1, ld_sounds_name(edit_pad));
    draw_title_row(title);
}

static void draw_edit_tabs(void)
{
    row_t row;
    row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    put_span(&row, KITS_TAB, "KITS", edit_tab == TAB_KITS ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             edit_tab == TAB_KITS ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    put_span(&row, CARD_TAB, "SD CARD", edit_tab == TAB_CARD ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             edit_tab == TAB_CARD ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    if (ld_sounds_synth()->count > 0) {
        put_span(&row, SYNTH_TAB, "SYNTH", edit_tab == TAB_SYNTH ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                 edit_tab == TAB_SYNTH ? CYD_COLOR_CYAN : CYD_COLOR_BUTTON);
    }
    put_span(&row, OFF_TAB, "OFF", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(EDIT_TABS, &row);
    static const int gaps[] = {6, 14, 20};
    draw_gaps(EDIT_TABS, gaps, 3);
}

static void draw_kits(void)
{
    int kits = kit_count();
    for (int i = 0; i < EDIT_CONTENT_ROWS; ++i) {
        row_t row;
        row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
        int kit = kit_scroll + i;
        const ld_source_t *source = ld_sounds_source(edit_pad);
        if (kit < kits) {
            char name[8];
            snprintf(name, sizeof(name), "%.6s", kit_name(kit));
            put_text(&row, KIT_NAME_SPAN, name, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
            for (int track = 0; track < DRUM_TRACKS; ++track) {
                bool current = source->kind == LD_SOURCE_KIT && source->kit == kit &&
                               source->track == track;
                put_span(&row, kit_cell(track), kit_sound(kit, track)->label, CYD_COLOR_BLACK,
                         current ? CYD_COLOR_WHITE : track_colors[track]);
            }
        }
        if (kits > EDIT_CONTENT_ROWS && (i == 0 || i == EDIT_CONTENT_ROWS - 1)) {
            const char *arrow = i == 0 ? "\x01" : "\x02";
            put_span(&row, SCROLL_SPAN, arrow, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
        }
        draw_row(EDIT_CONTENT + i, &row);
        if (kit < kits) {
            static const int gaps[] = {8, 10, 12, 14, 16, 18, 20, 22};
            draw_gaps(EDIT_CONTENT + i, gaps, 8);
        }
    }
}

static bool card_at_root(void)
{
    return strcmp(card_folder, SDCARD_ROOT) == 0;
}

static void list_card(void)
{
    if (card_entries == NULL) {
        card_entries = heap_caps_malloc(CARD_ENTRIES * sizeof(ld_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (card_entries == NULL) {
            card_count = 0;
            return;
        }
    }
    if (card_folder[0] == '\0') {
        card_count = ld_sounds_list(LD_SAMPLE_DIR, card_entries, CARD_ENTRIES);
        snprintf(card_folder, sizeof(card_folder), "%s", card_count > 0 ? LD_SAMPLE_DIR : SDCARD_ROOT);
    }
    card_count = ld_sounds_list(card_folder, card_entries, CARD_ENTRIES);
    card_scroll = clamp(card_scroll, 0, card_count > CARD_LIST_ROWS ? card_count - CARD_LIST_ROWS : 0);
    card_listed = true;
}

static void card_entry_path(int entry, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", card_folder, card_entries[entry].name);
}

static void draw_card(void)
{
    if (!card_listed) {
        list_card();
    }
    row_t row;
    row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    if (!sdcard_mounted()) {
        draw_text_row(EDIT_CONTENT, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, " No SD card. Put one in,");
        draw_text_row(EDIT_CONTENT + 1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, " then tap SD CARD again.");
        for (int i = 2; i < EDIT_CONTENT_ROWS; ++i) {
            draw_text_row(EDIT_CONTENT + i, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "");
        }
        return;
    }
    /* The folder, its last part if it is long, and UP. */
    const char *shown = card_folder + strlen(SDCARD_ROOT);
    size_t length = strlen(shown);
    if (length + 1 > (size_t)CARD_PATH_SPAN.width) {
        shown += length + 1 - CARD_PATH_SPAN.width;
    }
    char path[COLUMNS + 1];
    snprintf(path, sizeof(path), " %s", shown[0] != '\0' ? shown : "/");
    put_text(&row, CARD_PATH_SPAN, path, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(&row, CARD_UP_SPAN, "UP", CYD_COLOR_WHITE,
             card_at_root() ? CYD_COLOR_BUTTON_DISABLED : CYD_COLOR_BUTTON);
    draw_row(EDIT_CONTENT, &row);

    const ld_source_t *source = ld_sounds_source(edit_pad);
    for (int i = 0; i < CARD_LIST_ROWS; ++i) {
        row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
        int entry = card_scroll + i;
        if (entry < card_count) {
            char text[LD_ENTRY_MAX + 4];
            char full[LD_PATH_MAX];
            card_entry_path(entry, full, sizeof(full));
            bool folder = card_entries[entry].folder;
            bool current = !folder && source->kind == LD_SOURCE_WAV && strcmp(source->path, full) == 0;
            snprintf(text, sizeof(text), " %s%s", card_entries[entry].name, folder ? "/" : "");
            put_text(&row, CARD_ENTRY_SPAN, text,
                     current ? CYD_COLOR_BLACK : folder ? CYD_COLOR_CYAN : CYD_COLOR_WHITE,
                     current ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON);
        } else if (i == 0 && card_count == 0) {
            put_text(&row, CARD_ENTRY_SPAN, " No WAVs or folders here", CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
        }
        if (card_count > CARD_LIST_ROWS && (i == 0 || i == CARD_LIST_ROWS - 1)) {
            put_span(&row, SCROLL_SPAN, i == 0 ? "\x01" : "\x02", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
        }
        draw_row(CARD_LIST + i, &row);
    }
}

static void draw_synth(void)
{
    const ld_synth_source_t *synth = ld_sounds_synth();
    const ld_source_t *source = ld_sounds_source(edit_pad);
    for (int i = 0; i < EDIT_CONTENT_ROWS; ++i) {
        row_t row;
        row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
        if (i < synth->count) {
            uint32_t frames = 0;
            const int16_t *pcm = synth->pcm(i, &frames);
            char text[COLUMNS + 1];
            if (pcm != NULL && frames > 0) {
                snprintf(text, sizeof(text), " %-14s %.1f S", synth->name(i), (double)frames / LD_RATE);
            } else {
                snprintf(text, sizeof(text), " S%d  EMPTY: RENDER IN SYNTH", i + 1);
            }
            bool current = source->kind == LD_SOURCE_SYNTH && source->synth == i;
            put_text(&row, (span_t){0, COLUMNS}, text,
                     current ? CYD_COLOR_BLACK : pcm != NULL ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
                     current ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON);
        }
        draw_row(EDIT_CONTENT + i, &row);
    }
}

static void draw_edit_content(void)
{
    switch (edit_tab) {
    case TAB_CARD:
        draw_card();
        break;
    case TAB_SYNTH:
        draw_synth();
        break;
    default:
        draw_kits();
        break;
    }
}

static void draw_edit_settings(void)
{
    const ld_pad_settings_t *settings = ld_pad(edit_pad);
    char value[COLUMNS + 1];
    snprintf(value, sizeof(value), "LEVEL %u", settings->level);
    draw_split_row(EDIT_LEVEL, "LVL -", value, "LVL +");
    snprintf(value, sizeof(value), "PITCH %+d", settings->pitch);
    draw_split_row(EDIT_PITCH, "PIT -", value, "PIT +");
    row_t row;
    row_init(&row, CYD_COLOR_WHITE, CYD_COLOR_BLACK);
    put_span(&row, NAV_PREV, "<PAD", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(&row, NAV_PLAY, "PLAY", CYD_COLOR_WHITE, COLOR_PLAYING);
    char choke[COLUMNS + 1];
    if (settings->choke == 0) {
        snprintf(choke, sizeof(choke), "CHOKE OFF");
    } else {
        snprintf(choke, sizeof(choke), "CHOKE %u", settings->choke);
    }
    put_span(&row, NAV_CHOKE, choke, settings->choke ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
             settings->choke ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON);
    put_span(&row, NAV_NEXT, "PAD>", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    draw_row(EDIT_NAV, &row);
    static const int gaps[] = {5, 12, 21};
    draw_gaps(EDIT_NAV, gaps, 3);
}

static void draw_editor(void)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    draw_edit_title();
    draw_text_row(1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text);
    draw_edit_tabs();
    draw_edit_content();
    draw_edit_settings();
}

/* After the pad's sound changed: its name, the highlight, and a hit. */
static void assigned(esp_err_t result, const char *status)
{
    set_status(status);
    if (result == ESP_OK) {
        ld_hit(edit_pad);
        draw_edit_title();
    }
    draw_edit_content();
}

static void tap_kits(int row, int column)
{
    int kits = kit_count();
    if (kits > EDIT_CONTENT_ROWS && in_span(SCROLL_SPAN, column)) {
        if (row == 0) {
            kit_scroll = clamp(kit_scroll - EDIT_CONTENT_ROWS + 1, 0, kits - EDIT_CONTENT_ROWS);
        } else if (row == EDIT_CONTENT_ROWS - 1) {
            kit_scroll = clamp(kit_scroll + EDIT_CONTENT_ROWS - 1, 0, kits - EDIT_CONTENT_ROWS);
        }
        draw_kits();
        return;
    }
    int kit = kit_scroll + row;
    if (kit >= kits || column < KIT_NAME_SPAN.width || column >= kit_cell(DRUM_TRACKS).column) {
        return;
    }
    int track = (column - KIT_NAME_SPAN.width) / 2;
    char status[COLUMNS + 1];
    if (kit_on_card(kit)) {
        snprintf(status, sizeof(status), " Loading %s...", kit_name(kit));
        set_status(status);
    }
    esp_err_t result = ld_sounds_assign_kit(edit_pad, kit, track, status, sizeof(status));
    assigned(result, status);
}

static void tap_card(int row, int column)
{
    if (!sdcard_mounted()) {
        return;
    }
    if (row == 0) {
        if (in_span(CARD_UP_SPAN, column) && !card_at_root()) {
            char *slash = strrchr(card_folder, '/');
            if (slash != NULL) {
                *slash = '\0';
            }
            if (strlen(card_folder) < strlen(SDCARD_ROOT)) {
                snprintf(card_folder, sizeof(card_folder), "%s", SDCARD_ROOT);
            }
            card_scroll = 0;
            list_card();
            draw_card();
        }
        return;
    }
    int line = row - (CARD_LIST - EDIT_CONTENT);
    if (card_count > CARD_LIST_ROWS && in_span(SCROLL_SPAN, column)) {
        int page = CARD_LIST_ROWS - 1;
        if (line == 0) {
            card_scroll = clamp(card_scroll - page, 0, card_count - CARD_LIST_ROWS);
        } else if (line == CARD_LIST_ROWS - 1) {
            card_scroll = clamp(card_scroll + page, 0, card_count - CARD_LIST_ROWS);
        }
        draw_card();
        return;
    }
    int entry = card_scroll + line;
    if (entry < 0 || entry >= card_count || column >= CARD_ENTRY_SPAN.width) {
        return;
    }
    char path[LD_PATH_MAX];
    card_entry_path(entry, path, sizeof(path));
    if (strlen(path) >= LD_PATH_MAX - 1) {
        set_status(" The path is too long");
        return;
    }
    if (card_entries[entry].folder) {
        snprintf(card_folder, sizeof(card_folder), "%s", path);
        card_scroll = 0;
        list_card();
        draw_card();
        return;
    }
    char status[COLUMNS + 1];
    snprintf(status, sizeof(status), " Loading %s...", card_entries[entry].name);
    set_status(status);
    esp_err_t result = ld_sounds_assign_wav(edit_pad, path, status, sizeof(status));
    assigned(result, status);
}

static void tap_synth(int row)
{
    if (row >= ld_sounds_synth()->count) {
        return;
    }
    char status[COLUMNS + 1];
    esp_err_t result = ld_sounds_assign_synth(edit_pad, row, status, sizeof(status));
    assigned(result, status);
}

static void tap_tabs(int column)
{
    edit_tab_t tab = edit_tab;
    if (in_span(KITS_TAB, column)) {
        tab = TAB_KITS;
    } else if (in_span(CARD_TAB, column)) {
        tab = TAB_CARD;
        if (!sdcard_mounted() || edit_tab == TAB_CARD) {
            set_status(" Looking at the SD card...");
            card_listed = false; /* list it again: a card may be new */
        }
    } else if (in_span(SYNTH_TAB, column) && ld_sounds_synth()->count > 0) {
        tab = TAB_SYNTH;
    } else if (in_span(OFF_TAB, column)) {
        ld_sounds_clear(edit_pad);
        char status[COLUMNS + 1];
        snprintf(status, sizeof(status), " Pad %d is off", edit_pad + 1);
        set_status(status);
        draw_edit_title();
        draw_edit_content();
        return;
    } else {
        return;
    }
    edit_tab = tab;
    draw_edit_tabs();
    draw_edit_content();
    if (tab == TAB_CARD) {
        set_status(sdcard_mounted() ? " Tap a WAV to put it on the pad" : " No SD card");
    }
}

static void tap_settings(int row, int column)
{
    ld_pad_settings_t *settings = ld_pad(edit_pad);
    bool left = column < 6;
    bool right = column >= 20;
    if (row == EDIT_LEVEL && (left || right)) {
        settings->level = (uint8_t)clamp(settings->level + (left ? -LEVEL_STEP : LEVEL_STEP), 0, LD_LEVEL_MAX);
        ld_hit(edit_pad);
    } else if (row == EDIT_PITCH && (left || right)) {
        settings->pitch = (int8_t)clamp(settings->pitch + (left ? -1 : 1), LD_PITCH_MIN, LD_PITCH_MAX);
        ld_hit(edit_pad);
    } else if (row == EDIT_NAV) {
        if (in_span(NAV_PLAY, column)) {
            ld_hit(edit_pad);
            return;
        }
        if (in_span(NAV_CHOKE, column)) {
            settings->choke = (uint8_t)((settings->choke + 1) % (LD_CHOKE_GROUPS + 1));
        } else {
            int step = in_span(NAV_PREV, column) ? -1 : 1;
            edit_pad = (edit_pad + step + layout) % layout;
            draw_edit_title();
            draw_edit_content();
        }
    }
    draw_edit_settings();
}

/* The pad's editor, until BACK. Taps come one finger at a time. */
static void run_editor(QueueHandle_t touch_queue, int pad)
{
    edit_pad = pad;
    pads_live = false;
    cyd_touch_set_fast(false);
    if (sdcard_mounted()) {
        kit_scan(KIT_FOLDER); /* kits put on the card since boot */
    }
    if (edit_tab == TAB_SYNTH && ld_sounds_synth()->count == 0) {
        edit_tab = TAB_KITS;
    }
    card_listed = false;
    snprintf(status_text, STATUS_SIZE, " Tap a sound to put it on pad %d", pad + 1);
    draw_editor();
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (music_out_check_lost()) {
                speaker_lost_notice = true;
            }
            save_tempo_when_settled();
            update_flashes(); /* keeps the heard queue drained */
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        if (row == 0) {
            if (column >= 22) {
                return;
            }
        } else if (row == EDIT_TABS) {
            tap_tabs(column);
        } else if (row >= EDIT_CONTENT && row < EDIT_CONTENT + EDIT_CONTENT_ROWS) {
            if (edit_tab == TAB_KITS) {
                tap_kits(row - EDIT_CONTENT, column);
            } else if (edit_tab == TAB_CARD) {
                tap_card(row - EDIT_CONTENT, column);
            } else {
                tap_synth(row - EDIT_CONTENT);
            }
        } else if (row >= EDIT_LEVEL) {
            tap_settings(row, column);
        }
    }
}

/* ---- Banks -------------------------------------------------------------------- */

#define BANK_FIRST 2
#define BANK_DEFAULT 10
#define BANK_BACK 11

typedef enum {
    ARM_NONE,
    ARM_LOAD,
    ARM_SAVE,
    ARM_DEFAULT,
} arm_t;

static void draw_bank_row(int bank, arm_t armed)
{
    char info[COLUMNS + 1];
    if (!ld_bank_info(bank, info, sizeof(info))) {
        snprintf(info, sizeof(info), "NO CARD");
    }
    char label[COLUMNS + 1];
    snprintf(label, sizeof(label), "%d %s", bank + 1, info);
    draw_split_row(BANK_FIRST + bank, armed == ARM_LOAD ? "LOAD?" : "LOAD", label,
                   armed == ARM_SAVE ? "SAVE?" : "SAVE");
}

static void draw_default_row(bool armed)
{
    draw_text_row(BANK_DEFAULT, CYD_COLOR_WHITE, armed ? CYD_COLOR_RED : CYD_COLOR_BUTTON,
                  armed ? " TAP AGAIN: DEFAULT PADS" : " DEFAULT PADS");
}

static void draw_banks(void)
{
    draw_title_row(" BANKS ON THE SD CARD");
    draw_text_row(1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text);
    for (int bank = 0; bank < LD_BANKS; ++bank) {
        draw_bank_row(bank, ARM_NONE);
    }
    draw_default_row(false);
    draw_text_row(BANK_BACK, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " BACK TO PADS");
}

/* Load and save the pads and loop, until BACK. LOAD and SAVE each take a
   second tap. */
static void run_banks(QueueHandle_t touch_queue)
{
    pads_live = false;
    cyd_touch_set_fast(false);
    snprintf(status_text, STATUS_SIZE, " Pads, levels and loop: tap twice");
    draw_banks();
    xQueueReset(touch_queue);
    int armed_bank = -1;
    arm_t armed = ARM_NONE;
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (music_out_check_lost()) {
                speaker_lost_notice = true;
            }
            save_tempo_when_settled();
            update_flashes();
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        bool left = column < 6;
        bool right = column >= 20;
        int bank = row - BANK_FIRST;
        arm_t action = ARM_NONE;
        if (bank >= 0 && bank < LD_BANKS && (left || right)) {
            action = left ? ARM_LOAD : ARM_SAVE;
        } else if (row == BANK_DEFAULT) {
            action = ARM_DEFAULT;
            bank = -1;
        }
        bool confirmed = action != ARM_NONE && action == armed && bank == armed_bank;
        /* Disarm what was armed. */
        if (armed == ARM_DEFAULT) {
            draw_default_row(false);
        } else if (armed != ARM_NONE) {
            draw_bank_row(armed_bank, ARM_NONE);
        }
        armed = ARM_NONE;
        armed_bank = -1;
        if ((row == 0 && column >= 22) || row == BANK_BACK) {
            return;
        }
        if (action == ARM_NONE) {
            continue;
        }
        if (!confirmed) {
            armed = action;
            armed_bank = bank;
            if (action == ARM_DEFAULT) {
                draw_default_row(true);
                set_status(" The loop stays");
            } else {
                draw_bank_row(bank, action);
                set_status(action == ARM_LOAD ? " Tap LOAD? to load" : " Tap SAVE? to save");
            }
            continue;
        }
        char status[COLUMNS + 1];
        if (action == ARM_DEFAULT) {
            ld_sounds_default();
            set_status(" Default pads");
            continue;
        }
        set_status(action == ARM_LOAD ? " Loading..." : " Saving...");
        if (action == ARM_LOAD) {
            uint16_t bpm = host->tempo();
            if (ld_bank_load(bank, &bpm, status, sizeof(status)) == ESP_OK && bpm != host->tempo()) {
                host->set_tempo(bpm);
            }
        } else {
            ld_bank_save(bank, host->tempo(), status, sizeof(status));
            draw_bank_row(bank, ARM_NONE);
        }
        set_status(status);
    }
}

/* ---- Menu --------------------------------------------------------------------- */

#define MENU_WIFI 2
#define MENU_OUTPUT 3
#define MENU_VOLUME 4
#define MENU_TEMPO 5
#define MENU_LAYOUT 6
#define MENU_BANKS 7
#define MENU_COUNT_IN 8
#define MENU_LATENCY 9
#define MENU_BACK 11
#define VOLUME_STEP 10
#define TEMPO_STEP 10

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
    snprintf(value, sizeof(value), "%u BPM", host->tempo());
    draw_split_row(MENU_TEMPO, "-10", value, "+10");
}

static void draw_layout_row(void)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " PADS ON SCREEN  %d", layout);
    draw_text_row(MENU_LAYOUT, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, line);
}

static void draw_count_in_row(void)
{
    if (host->has_count_in) {
        draw_text_row(MENU_COUNT_IN, CYD_COLOR_WHITE, CYD_COLOR_BUTTON,
                      count_in ? " REC COUNT-IN  1 BAR" : " REC COUNT-IN  OFF");
    }
}

static void draw_menu(void)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    draw_text_row(0, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, host->title);
    draw_text_row(1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text);
    music_out_draw_wifi_row(MENU_WIFI);
    music_out_draw_output_row(MENU_OUTPUT);
    draw_volume_row();
    draw_tempo_row();
    draw_layout_row();
    draw_text_row(MENU_BANKS, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " BANKS: LOAD AND SAVE");
    draw_count_in_row();
    music_out_draw_latency_row(MENU_LATENCY);
    draw_text_row(MENU_BACK, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " BACK TO PADS");
}

static void run_menu(QueueHandle_t touch_queue)
{
    if (speaker_lost_notice) {
        speaker_lost_notice = false;
        snprintf(status_text, STATUS_SIZE, " Speaker disconnected");
    }
    pads_live = false;
    cyd_touch_set_fast(false);
    draw_menu();
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (music_out_check_lost()) {
                set_status(" Speaker disconnected");
                music_out_draw_output_row(MENU_OUTPUT);
                draw_volume_row();
            }
            save_tempo_when_settled();
            update_flashes();
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        bool left = column < 6;
        bool right = column >= 20;
        char status[COLUMNS + 1];
        switch (row) {
        case 0:
        case MENU_BACK:
            return;
        case MENU_WIFI:
            if (music_out_tap_wifi(status, sizeof(status))) {
                snprintf(status_text, STATUS_SIZE, "%s", status);
                draw_menu();
                xQueueReset(touch_queue);
            } else {
                set_status(status);
            }
            break;
        case MENU_OUTPUT:
            music_out_pick_speaker(status, sizeof(status));
            snprintf(status_text, STATUS_SIZE, "%s", status);
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
                host->set_tempo(host->tempo() + (left ? -TEMPO_STEP : TEMPO_STEP));
                draw_tempo_row();
            }
            break;
        case MENU_LAYOUT:
            layout = layout == 8 ? 12 : layout == 12 ? 16 : 8;
            save_prefs();
            draw_layout_row();
            set_status(layout < LD_PADS ? " The other pads keep their sounds" : " All 16 pads");
            break;
        case MENU_BANKS:
            run_banks(touch_queue);
            draw_menu();
            xQueueReset(touch_queue);
            break;
        case MENU_COUNT_IN:
            if (host->has_count_in) {
                count_in = !count_in;
                save_prefs();
                draw_count_in_row();
            }
            break;
        case MENU_LATENCY:
            music_out_next_latency_setting();
            music_out_draw_latency_row(MENU_LATENCY);
            set_status(" Used from the next connect");
            break;
        default:
            break;
        }
    }
}

/* ---- Pads screen ------------------------------------------------------------- */

static void tap_header(int column, QueueHandle_t touch_queue, bool *leave)
{
    if (in_span(PLAY_SPAN, column)) {
        host->play(!host->is_playing(), 0);
    } else if (in_span(REC_SPAN, column)) {
        if (ld_is_recording()) {
            ld_record(false);
        } else {
            ld_record(true);
            if (!host->is_playing()) {
                host->play(true, host->has_count_in && count_in ? 1 : 0);
            }
        }
    } else if (in_span(BPM_DOWN_SPAN, column)) {
        host->set_tempo(host->tempo() - 1);
    } else if (in_span(BPM_UP_SPAN, column)) {
        host->set_tempo(host->tempo() + 1);
    } else if (in_span(EDIT_SPAN, column)) {
        edit_mode = !edit_mode;
    } else if (in_span(SWITCH_SPAN, column)) {
        if (host->switch_label != NULL) {
            *leave = true;
        } else {
            run_banks(touch_queue);
        }
    } else if (in_span(MENU_SPAN, column)) {
        run_menu(touch_queue);
    }
}

static void tap_strip(int column)
{
    bool was_armed = clear_armed;
    clear_armed = false;
    if (in_span(BARS_SPAN, column)) {
        ld_cycle_bars();
    } else if (in_span(QUANT_SPAN, column)) {
        /* 1/16, 1/32, triplets, off, 1/8 and round. */
        static const uint8_t next[LD_QUANTIZE_MODES] = {
            [LD_QUANTIZE_16] = LD_QUANTIZE_32, [LD_QUANTIZE_32] = LD_QUANTIZE_8T,
            [LD_QUANTIZE_8T] = LD_QUANTIZE_16T, [LD_QUANTIZE_16T] = LD_QUANTIZE_OFF,
            [LD_QUANTIZE_OFF] = LD_QUANTIZE_8,  [LD_QUANTIZE_8] = LD_QUANTIZE_16,
        };
        ld_quantize = next[clamp(ld_quantize, 0, LD_QUANTIZE_MODES - 1)];
    } else if (in_span(MET_SPAN, column)) {
        ld_metronome = !ld_metronome;
    } else if (in_span(UNDO_SPAN, column)) {
        ld_undo();
    } else if (in_span(CLEAR_SPAN, column)) {
        if (was_armed) {
            ld_clear();
        } else if (ld_hit_count() > 0) {
            clear_armed = true;
            clear_armed_at = xTaskGetTickCount();
        }
    }
}

/* A pad held still while the loop is stopped opens its editor. */
static int held_pad(void)
{
    static unsigned seen_taps;
    static int candidate = -1;
    static TickType_t since;
    unsigned taps = atomic_load(&tap_count);
    if (taps != seen_taps) {
        seen_taps = taps;
        candidate = tap_pad;
        since = tap_at;
    }
    if (candidate < 0 || edit_mode || host->is_playing()) {
        candidate = -1;
        return -1;
    }
    cyd_touch_point_t held[CYD_TOUCH_MAX_FINGERS];
    int count = cyd_touch_held(held, CYD_TOUCH_MAX_FINGERS);
    bool still = false;
    for (int i = 0; i < count; ++i) {
        still = still || pad_at(held[i].x, held[i].y) == candidate;
    }
    if (!still) {
        candidate = -1;
        return -1;
    }
    if (xTaskGetTickCount() - since < pdMS_TO_TICKS(HOLD_MS)) {
        return -1;
    }
    int pad = candidate;
    candidate = -1;
    return pad;
}

/* The pads screen: fast multi-finger taps, pads played by on_tap. */
static void enter_pads(QueueHandle_t touch_queue)
{
    full_redraw();
    xQueueReset(touch_queue);
    atomic_store(&live_hits, 0);
    pads_live = true;
    cyd_touch_set_fast(true);
}

static void init_colors(void)
{
    track_colors[DRUM_KICK] = cyd_rgb(255, 70, 40);
    track_colors[DRUM_SNARE] = cyd_rgb(255, 200, 0);
    track_colors[DRUM_CLAP] = cyd_rgb(255, 130, 0);
    track_colors[DRUM_CLOSED_HAT] = cyd_rgb(0, 210, 255);
    track_colors[DRUM_OPEN_HAT] = cyd_rgb(70, 130, 255);
    track_colors[DRUM_TOM] = cyd_rgb(60, 220, 90);
    track_colors[DRUM_RIM] = cyd_rgb(230, 90, 230);
    track_colors[DRUM_EXTRA] = cyd_rgb(220, 220, 200);
}

void ld_screen(QueueHandle_t touch_queue, const ld_host_t *screen_host)
{
    static bool started;
    host = screen_host;
    status_text = host->status != NULL ? host->status : own_status;
    if (!started) {
        started = true;
        init_colors();
        load_prefs();
        saved_bpm = changed_bpm = host->tempo();
    }
    cyd_touch_set_fingers_callback(NULL, NULL);
    cyd_touch_set_tap_callback(on_tap, NULL);
    enter_pads(touch_queue);
    bool leave = false;
    while (!leave) {
        if (music_out_check_lost()) {
            speaker_lost_notice = true;
        }
        save_tempo_when_settled();
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        bool reenter = false;
        /* Take every tap waiting before drawing, so pads answer first. */
        while (!leave && !reenter && xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            int pad = pad_at(point.x, point.y);
            if (point.y < FH) {
                bool was_live = pads_live;
                tap_header(point.x / FW, touch_queue, &leave);
                reenter = was_live && !pads_live; /* a menu or the banks ran */
            } else if (point.y >= PAD_BOTTOM) {
                tap_strip(point.x / FW);
            } else if (pad >= 0 && pad < layout) {
                if (edit_mode) {
                    edit_mode = false;
                    run_editor(touch_queue, pad);
                    reenter = true;
                } else {
                    ld_hit(pad); /* only if it came before pads_live */
                    flash(pad, true);
                }
            }
        }
        if (leave) {
            break;
        }
        if (!reenter) {
            int pad = held_pad();
            if (pad >= 0) {
                run_editor(touch_queue, pad);
                reenter = true;
            }
        }
        if (reenter) {
            enter_pads(touch_queue);
            continue;
        }
        update_flashes();
        refresh();
    }
    pads_live = false;
    cyd_touch_set_tap_callback(NULL, NULL);
}
