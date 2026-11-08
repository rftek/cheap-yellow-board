/* Performance screen, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 140 +, bar page, MENU
     rows 1-8    the step grid: a track per row, its label (tap to mute)
                 and 16 steps of the bar on view (tap to toggle)
     rows 9-11   eight pads that play the sounds live

   The grid's playhead follows what the speaker is playing, which is the
   AirPlay latency behind what the board renders. */

#include "ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cyd_display.h"
#include "export.h"
#include "nvs.h"
#include "output.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

_Static_assert(CYD_LCD_WIDTH == 800 && CYD_TEXT_ROWS == 12,
               "DrumLoop is laid out for the 800x480 Waveshare S3 board");

#define FW CYD_FONT_WIDTH
#define FH CYD_FONT_HEIGHT
#define COLUMNS CYD_TEXT_COLUMNS

#define GRID_ROW 1
#define PAD_ROW 9
#define PAD_LABEL_ROW 10
#define LABEL_WIDTH 128
#define STEP_WIDTH ((CYD_LCD_WIDTH - LABEL_WIDTH) / DRUM_STEPS)
#define PAD_WIDTH (CYD_LCD_WIDTH / DRUM_TRACKS)
#define PAD_GAP 6
#define PAD_TOP (PAD_ROW * FH)

#define REFRESH_MS 10
#define FLASH_MS 120
#define NO_STEP -1

#define COLOR_OFF 0x2104
#define COLOR_BEAT 0x39C7
#define COLOR_HEAD 0x7BEF
#define COLOR_PLAYING 0x0560
#define COLOR_MUTED CYD_COLOR_BUTTON_DISABLED

#define SETTINGS_NAMESPACE "drumloop"
#define SETTINGS_LATENCY "latency_ms"
#define DEFAULT_LATENCY_MS 2000

typedef struct {
    int column;
    int width;
} span_t;

static const span_t PLAY_SPAN = {0, 4};
static const span_t REC_SPAN = {5, 3};
static const span_t BPM_DOWN_SPAN = {9, 2};
static const span_t BPM_SPAN = {11, 3};
static const span_t BPM_UP_SPAN = {14, 2};
static const span_t BAR_SPAN = {17, 5};
static const span_t MENU_SPAN = {22, 4};

static uint16_t track_colors[DRUM_TRACKS];

/* Screen state, and what is drawn now, so a refresh redraws only changes. */
static drum_pattern_t *pattern;
static volatile bool recording;
static bool follow = true;
static int view_bar;
static int playhead = NO_STEP;
static TickType_t flash_until[DRUM_TRACKS];
/* Pads are played from the touch task (on_tap) while the performance
   screen is up; it leaves the pads it played here for this task to flash. */
static volatile bool pads_live;
static atomic_uint pads_hit;
/* The speaker dropped the session and output.c went silent: the MENU
   button is red until the menu says so. */
static bool speaker_lost_notice;

static char drawn_header[COLUMNS + 1];
static uint16_t drawn_header_fg[COLUMNS];
static uint16_t drawn_header_bg[COLUMNS];
static uint32_t drawn_cells[DRUM_TRACKS][DRUM_STEPS];
static int drawn_muted;
static int drawn_flash;

#define NOT_DRAWN 0xFFFFFFFFu

static uint16_t dim(uint16_t color)
{
    return (uint16_t)((color >> 1) & 0x7BEF);
}

static bool in_span(span_t span, int column)
{
    return column >= span.column && column < span.column + span.width;
}

/* Writes text centered in a span of a row being composed. */
static void put_span(char *text, uint16_t *fg, uint16_t *bg, span_t span, const char *label,
                     uint16_t foreground, uint16_t background)
{
    int length = (int)strlen(label);
    if (length > span.width) {
        length = span.width;
    }
    int start = span.column + (span.width - length) / 2;
    for (int column = span.column; column < span.column + span.width; ++column) {
        fg[column] = foreground;
        bg[column] = background;
    }
    memcpy(text + start, label, (size_t)length);
}

/* ---- Settings ----------------------------------------------------------- */

uint32_t ui_saved_latency_ms(void)
{
    uint32_t latency = DEFAULT_LATENCY_MS;
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, SETTINGS_LATENCY, &latency);
        nvs_close(nvs);
    }
    return latency;
}

static void save_latency_ms(uint32_t latency)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, SETTINGS_LATENCY, latency);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ---- Starter beats ------------------------------------------------------ */

static void load_beat(uint16_t bpm, int kit, const uint16_t steps[DRUM_TRACKS])
{
    pattern->kit = (uint8_t)kit;
    memset(pattern->steps, 0, sizeof(pattern->steps));
    memcpy(pattern->steps[0], steps, sizeof(pattern->steps[0]));
    pattern->bars = 1;
    pattern->bpm = bpm;
    pattern->muted = 0;
    view_bar = 0;
}

/* Half-time trap at 140: kick and 808 on 1 and around, snare and clap on
   3, eighth-note hats with a sixteenth roll into an open hat. */
static void load_trap(void)
{
    static const uint16_t steps[DRUM_TRACKS] = {
        [DRUM_KICK] = 0x0481,       /* 0, 7, 10 */
        [DRUM_SNARE] = 0x0100,      /* 8 */
        [DRUM_CLAP] = 0x0100,       /* 8 */
        [DRUM_CLOSED_HAT] = 0xB555, /* eighths, 13 and 15, not 14 */
        [DRUM_OPEN_HAT] = 0x4000,   /* 14 */
    };
    load_beat(140, KIT_TRAP, steps);
}

/* Two-step drum and bass at 174: kick on 1 and the "and" of 3, snare on 2
   and 4, eighth-note hats and ghost rim shots. */
static void load_dnb(void)
{
    static const uint16_t steps[DRUM_TRACKS] = {
        [DRUM_KICK] = 0x0401,       /* 0, 10 */
        [DRUM_SNARE] = 0x1010,      /* 4, 12 */
        [DRUM_CLOSED_HAT] = 0x5555, /* eighths */
        [DRUM_RIM] = 0x8080,        /* 7, 15 */
    };
    load_beat(174, KIT_DNB, steps);
}

/* ---- Drawing ------------------------------------------------------------ */

static void invalidate(void)
{
    memset(drawn_header, 0, sizeof(drawn_header));
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        for (int step = 0; step < DRUM_STEPS; ++step) {
            drawn_cells[track][step] = NOT_DRAWN;
        }
    }
    drawn_muted = -1;
    drawn_flash = -1;
}

static void refresh_header(void)
{
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
    memset(text, ' ', COLUMNS);
    text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        fg[column] = CYD_COLOR_WHITE;
        bg[column] = CYD_COLOR_BLACK;
    }
    bool playing = audio_is_playing();
    put_span(text, fg, bg, PLAY_SPAN, playing ? "STOP" : "PLAY", CYD_COLOR_WHITE,
             playing ? COLOR_PLAYING : CYD_COLOR_BUTTON);
    put_span(text, fg, bg, REC_SPAN, "REC", CYD_COLOR_WHITE,
             recording ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    put_span(text, fg, bg, BPM_DOWN_SPAN, "-", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char number[8];
    snprintf(number, sizeof(number), "%u", pattern->bpm);
    put_span(text, fg, bg, BPM_SPAN, number, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(text, fg, bg, BPM_UP_SPAN, "+", CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    char bar[8];
    snprintf(bar, sizeof(bar), "%d/%u", view_bar + 1, pattern->bars);
    uint16_t bar_bg = pattern->bars == 1 ? CYD_COLOR_BUTTON_DISABLED
                      : follow           ? CYD_COLOR_CYAN
                                         : CYD_COLOR_BUTTON;
    put_span(text, fg, bg, BAR_SPAN, bar,
             follow && pattern->bars > 1 ? CYD_COLOR_BLACK : CYD_COLOR_WHITE, bar_bg);
    put_span(text, fg, bg, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);

    if (strcmp(text, drawn_header) == 0 && memcmp(fg, drawn_header_fg, sizeof(fg)) == 0 &&
        memcmp(bg, drawn_header_bg, sizeof(bg)) == 0) {
        return;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(0, text, fg, bg));
    memcpy(drawn_header, text, sizeof(drawn_header));
    memcpy(drawn_header_fg, fg, sizeof(fg));
    memcpy(drawn_header_bg, bg, sizeof(bg));
}

/* A track's row: its label, and black under the steps (redrawn after). */
static void draw_track_label(int track)
{
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
    memset(text, ' ', COLUMNS);
    text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        fg[column] = CYD_COLOR_WHITE;
        bg[column] = CYD_COLOR_BLACK;
    }
    bool muted = (pattern->muted >> track) & 1;
    span_t label = {0, LABEL_WIDTH / FW};
    put_span(text, fg, bg, label, kit_sound(pattern->kit, track)->label,
             muted ? CYD_COLOR_GRAY : CYD_COLOR_BLACK, muted ? COLOR_MUTED : track_colors[track]);
    ESP_ERROR_CHECK(cyd_draw_cells(GRID_ROW + track, text, fg, bg));
    for (int step = 0; step < DRUM_STEPS; ++step) {
        drawn_cells[track][step] = NOT_DRAWN;
    }
}

static uint16_t cell_color(int track, int step)
{
    bool on = (pattern->steps[view_bar][track] >> step) & 1;
    bool muted = (pattern->muted >> track) & 1;
    bool head = playhead != NO_STEP && playhead / DRUM_STEPS == view_bar &&
                playhead % DRUM_STEPS == step;
    if (on) {
        return head ? CYD_COLOR_WHITE : muted ? dim(track_colors[track]) : track_colors[track];
    }
    if (head) {
        return COLOR_HEAD;
    }
    return step % 4 == 0 ? COLOR_BEAT : COLOR_OFF;
}

static void refresh_grid(void)
{
    if (drawn_muted != pattern->muted) {
        for (int track = 0; track < DRUM_TRACKS; ++track) {
            if (drawn_muted < 0 || ((drawn_muted ^ pattern->muted) >> track) & 1) {
                draw_track_label(track);
            }
        }
        drawn_muted = pattern->muted;
    }
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        int y = (GRID_ROW + track) * FH;
        for (int step = 0; step < DRUM_STEPS; ++step) {
            uint16_t color = cell_color(track, step);
            if (drawn_cells[track][step] == color) {
                continue;
            }
            ESP_ERROR_CHECK(cyd_fill_rect(LABEL_WIDTH + step * STEP_WIDTH + 2, y + 2,
                                          STEP_WIDTH - 4, FH - 5, color));
            drawn_cells[track][step] = color;
        }
    }
}

static void draw_pads(int flash)
{
    uint16_t colors[DRUM_TRACKS];
    for (int pad = 0; pad < DRUM_TRACKS; ++pad) {
        colors[pad] = (flash >> pad) & 1 ? CYD_COLOR_WHITE : track_colors[pad];
        ESP_ERROR_CHECK(cyd_fill_rect(pad * PAD_WIDTH + PAD_GAP / 2, PAD_TOP + 2,
                                      PAD_WIDTH - PAD_GAP, CYD_LCD_HEIGHT - PAD_TOP - 4,
                                      colors[pad]));
    }
    /* Labels: a text row across all pads, each cell in the color of the pad
       under it; then the gaps between pads are cut back out. */
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
    memset(text, ' ', COLUMNS);
    text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        int pad = (column * FW + FW / 2) / PAD_WIDTH;
        fg[column] = CYD_COLOR_BLACK;
        bg[column] = colors[pad];
    }
    for (int pad = 0; pad < DRUM_TRACKS; ++pad) {
        int center = pad * PAD_WIDTH + PAD_WIDTH / 2;
        int start = (center - FW + FW / 2) / FW;
        memcpy(text + start, kit_sound(pattern->kit, pad)->label, 2);
    }
    ESP_ERROR_CHECK(cyd_draw_cells(PAD_LABEL_ROW, text, fg, bg));
    int label_y = PAD_LABEL_ROW * FH;
    for (int pad = 0; pad <= DRUM_TRACKS; ++pad) {
        int x = pad * PAD_WIDTH - PAD_GAP / 2;
        int width = PAD_GAP;
        if (x < 0) {
            width += x;
            x = 0;
        }
        if (x + width > CYD_LCD_WIDTH) {
            width = CYD_LCD_WIDTH - x;
        }
        ESP_ERROR_CHECK(cyd_fill_rect(x, label_y, width, FH, CYD_COLOR_BLACK));
    }
    /* cyd_draw_cells leaves the row's bottom pixel line black. */
    for (int pad = 0; pad < DRUM_TRACKS; ++pad) {
        ESP_ERROR_CHECK(cyd_fill_rect(pad * PAD_WIDTH + PAD_GAP / 2, label_y + FH - 1,
                                      PAD_WIDTH - PAD_GAP, 1, colors[pad]));
    }
}

static void refresh_pads(void)
{
    TickType_t now = xTaskGetTickCount();
    int flash = 0;
    for (int pad = 0; pad < DRUM_TRACKS; ++pad) {
        if ((int32_t)(flash_until[pad] - now) > 0) {
            flash |= 1 << pad;
        }
    }
    if (flash != drawn_flash) {
        draw_pads(flash);
        drawn_flash = flash;
    }
}

static void refresh(void)
{
    if (view_bar >= pattern->bars) {
        view_bar = 0;
    }
    refresh_header();
    refresh_grid();
    refresh_pads();
}

/* Moves the playhead to the step the speaker is playing now. */
static void update_playhead(void)
{
    if (!audio_is_playing()) {
        playhead = NO_STEP;
        return;
    }
    audio_step_t step;
    while (audio_next_heard_step(&step)) {
        playhead = step.step;
    }
    if (follow && playhead != NO_STEP && playhead / DRUM_STEPS < pattern->bars) {
        view_bar = playhead / DRUM_STEPS;
    }
}

/* ---- Menu --------------------------------------------------------------- */

#define MENU_INFO 1
#define MENU_WIFI 2
#define MENU_OUTPUT 3
#define MENU_VOLUME 4
#define MENU_TEMPO 5
#define MENU_LENGTH 6
#define MENU_BEATS 7
#define MENU_CLEAR 8 /* CLEAR on the left, EXPORT on the right */
#define MENU_KIT 9
#define MENU_LATENCY 10
#define MENU_BACK 11
#define SPLIT_BUTTON_COLUMNS 9
#define VOLUME_STEP 10
#define TEMPO_STEP 10

static const uint32_t latency_choices[] = {2000, 1000, 500, 250, 150, 100};

/* The menu's info line, kept between visits. */
static char status_text[COLUMNS + 1] = " No speaker: silent";

static void menu_status(const char *status)
{
    snprintf(status_text, sizeof(status_text), "%s", status);
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
}

/* A row with a button at each end and a value between them. */
static void draw_split_row(int row, const char *left, const char *value, const char *right)
{
    char text[COLUMNS + 1];
    uint16_t fg[COLUMNS];
    uint16_t bg[COLUMNS];
    memset(text, ' ', COLUMNS);
    text[COLUMNS] = '\0';
    for (int column = 0; column < COLUMNS; ++column) {
        fg[column] = CYD_COLOR_WHITE;
        bg[column] = CYD_COLOR_BLACK;
    }
    span_t left_span = {0, SPLIT_BUTTON_COLUMNS};
    span_t right_span = {COLUMNS - SPLIT_BUTTON_COLUMNS, SPLIT_BUTTON_COLUMNS};
    span_t middle = {SPLIT_BUTTON_COLUMNS, COLUMNS - 2 * SPLIT_BUTTON_COLUMNS};
    put_span(text, fg, bg, left_span, left, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    put_span(text, fg, bg, middle, value, CYD_COLOR_YELLOW, CYD_COLOR_BLACK);
    put_span(text, fg, bg, right_span, right, CYD_COLOR_WHITE, CYD_COLOR_BUTTON);
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, fg, bg));
}

static void draw_button_row(int row, const char *label)
{
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, label));
}

static void draw_wifi_row(void)
{
    char line[COLUMNS + 1];
    if (!output_wifi_saved()) {
        ESP_ERROR_CHECK(cyd_text_row(MENU_WIFI, CYD_COLOR_GRAY, CYD_COLOR_BUTTON_DISABLED,
                                     " WI-FI     NONE SAVED"));
        return;
    }
    if (!output_wifi_on()) {
        snprintf(line, sizeof(line), " WI-FI     OFF");
    } else if (output_wifi_joined()) {
        snprintf(line, sizeof(line), " WI-FI     ON  %s", output_wifi_ssid());
    } else {
        snprintf(line, sizeof(line), " WI-FI     NOT JOINED");
    }
    draw_button_row(MENU_WIFI, line);
}

static void draw_output_row(void)
{
    char line[COLUMNS + 1];
    const char *name = output_speaker_name();
    if (name != NULL) {
        snprintf(line, sizeof(line), " OUTPUT    %s", name);
    } else {
        snprintf(line, sizeof(line), " OUTPUT    NONE (SILENT)");
    }
    draw_button_row(MENU_OUTPUT, line);
}

static void draw_volume_row(void)
{
    char value[8];
    if (audio_has_speaker()) {
        snprintf(value, sizeof(value), "%d%%", audio_get_volume());
    } else {
        snprintf(value, sizeof(value), "SILENT");
    }
    draw_split_row(MENU_VOLUME, "VOL -", value, "VOL +");
}

static void draw_tempo_row(void)
{
    char value[12];
    snprintf(value, sizeof(value), "%u BPM", pattern->bpm);
    draw_split_row(MENU_TEMPO, "BPM -10", value, "BPM +10");
}

static void draw_length_row(void)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " LOOP LENGTH  %u BAR%s", pattern->bars,
             pattern->bars == 1 ? "" : "S");
    draw_button_row(MENU_LENGTH, line);
}

static void draw_clear_row(bool armed)
{
    draw_split_row(MENU_CLEAR, armed ? "CLEAR?" : "CLEAR", "", "EXPORT");
}

static void draw_kit_row(void)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " KIT  %s  %s", kit_name(pattern->kit), kit_about(pattern->kit));
    draw_button_row(MENU_KIT, line);
}

static void draw_latency_row(uint32_t latency)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " AIRPLAY DELAY %lu MS", (unsigned long)latency);
    draw_button_row(MENU_LATENCY, line);
}

static void draw_menu(uint32_t latency)
{
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, " DRUMLOOP SETTINGS"));
    ESP_ERROR_CHECK(cyd_text_row(MENU_INFO, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status_text));
    draw_wifi_row();
    draw_output_row();
    draw_volume_row();
    draw_tempo_row();
    draw_length_row();
    draw_split_row(MENU_BEATS, "TRAP 140", "LOAD BEAT", "DNB 174");
    draw_clear_row(false);
    draw_kit_row();
    draw_latency_row(latency);
    draw_button_row(MENU_BACK, " BACK TO PADS");
}

/* Doubles the loop (up to 16 bars), copying the bars it had into the new
   ones, so a beat can grow by editing the copies; from 16 back to 1. */
static void cycle_length(void)
{
    int bars = pattern->bars;
    if (bars >= DRUM_MAX_BARS) {
        pattern->bars = 1;
        return;
    }
    for (int bar = bars; bar < bars * 2; ++bar) {
        memcpy(pattern->steps[bar], pattern->steps[bar - bars], sizeof(pattern->steps[bar]));
    }
    pattern->bars = (uint8_t)(bars * 2);
}

static void export_loop(void)
{
    menu_status(" Rendering the loop...");
    drum_export_t export;
    esp_err_t result = export_render(pattern, &export);
    char line[COLUMNS + 1];
    if (result != ESP_OK) {
        snprintf(line, sizeof(line), " Render failed: %s", esp_err_to_name(result));
        menu_status(line);
        return;
    }
    result = export_save(&export, "drumloop.wav");
    if (result == ESP_ERR_NOT_SUPPORTED) {
        snprintf(line, sizeof(line), " %.1f s WAV; no save yet", (double)export.frames / DRUM_RATE);
    } else if (result == ESP_OK) {
        snprintf(line, sizeof(line), " Saved %.1f s WAV", (double)export.frames / DRUM_RATE);
    } else {
        snprintf(line, sizeof(line), " Save failed: %s", esp_err_to_name(result));
    }
    menu_status(line);
    export_free(&export);
}

static void run_menu(QueueHandle_t touch_queue)
{
    uint32_t latency = ui_saved_latency_ms();
    bool clear_armed = false;
    if (speaker_lost_notice) {
        speaker_lost_notice = false;
        snprintf(status_text, sizeof(status_text), " Speaker disconnected");
    }
    draw_menu(latency);
    /* Menu buttons get ordinary taps: one finger, with a gap between taps. */
    pads_live = false;
    cyd_touch_set_fast(false);
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (output_check_lost()) {
                menu_status(" Speaker disconnected");
                draw_output_row();
                draw_volume_row();
            }
            /* Keep up with the playhead, so the grid is current on return. */
            update_playhead();
            continue;
        }
        int row = point.y / FH;
        int column = point.x / FW;
        bool left = column < SPLIT_BUTTON_COLUMNS;
        bool right = column >= COLUMNS - SPLIT_BUTTON_COLUMNS;
        if (clear_armed && !(row == MENU_CLEAR && left)) {
            clear_armed = false;
            draw_clear_row(false);
        }
        char status[COLUMNS + 1];
        switch (row) {
        case 0:
        case MENU_BACK:
            return;
        case MENU_WIFI:
            if (!output_wifi_saved()) {
                menu_status(" Set Wi-Fi in the launcher");
            } else if (output_wifi_on()) {
                output_wifi_stop();
                menu_status(" Wi-Fi off");
                draw_wifi_row();
                draw_output_row();
                draw_volume_row();
            } else {
                output_wifi_start(status, sizeof(status));
                snprintf(status_text, sizeof(status_text), "%s", status);
                draw_menu(latency);
                xQueueReset(touch_queue);
            }
            break;
        case MENU_OUTPUT:
            output_pick_speaker(status, sizeof(status));
            snprintf(status_text, sizeof(status_text), "%s", status);
            draw_menu(latency);
            xQueueReset(touch_queue);
            break;
        case MENU_VOLUME:
            if ((left || right) && audio_has_speaker()) {
                audio_set_volume(audio_get_volume() + (left ? -VOLUME_STEP : VOLUME_STEP));
                draw_volume_row();
            }
            break;
        case MENU_TEMPO:
            if (left || right) {
                int bpm = pattern->bpm + (left ? -TEMPO_STEP : TEMPO_STEP);
                bpm = bpm < DRUM_BPM_MIN ? DRUM_BPM_MIN : bpm > DRUM_BPM_MAX ? DRUM_BPM_MAX : bpm;
                pattern->bpm = (uint16_t)bpm;
                draw_tempo_row();
            }
            break;
        case MENU_LENGTH:
            cycle_length();
            draw_length_row();
            break;
        case MENU_BEATS:
            if (left) {
                load_trap();
                menu_status(" Loaded the trap beat");
            } else if (right) {
                load_dnb();
                menu_status(" Loaded the DnB beat");
            } else {
                break;
            }
            draw_tempo_row();
            draw_length_row();
            draw_kit_row();
            break;
        case MENU_CLEAR:
            if (right) {
                export_loop();
            } else if (!left) {
                break;
            } else if (clear_armed) {
                memset(pattern->steps, 0, sizeof(pattern->steps));
                clear_armed = false;
                draw_clear_row(false);
                menu_status(" Pattern cleared");
            } else {
                clear_armed = true;
                draw_clear_row(true);
                menu_status(" Tap CLEAR? again to clear");
            }
            break;
        case MENU_KIT:
            /* Takes effect at once, even mid-loop; ringing hits finish on
               the kit they started on. */
            pattern->kit = (uint8_t)((pattern->kit + 1) % DRUM_KITS);
            draw_kit_row();
            break;
        case MENU_LATENCY: {
            size_t count = sizeof(latency_choices) / sizeof(latency_choices[0]);
            size_t next = 0;
            for (size_t i = 0; i < count; ++i) {
                if (latency_choices[i] == latency) {
                    next = (i + 1) % count;
                }
            }
            latency = latency_choices[next];
            save_latency_ms(latency);
            draw_latency_row(latency);
            menu_status(" Used from the next connect");
            break;
        }
        default:
            break;
        }
    }
}

/* ---- Performance screen ------------------------------------------------- */

static void tap_header(int column)
{
    if (in_span(PLAY_SPAN, column)) {
        bool play = !audio_is_playing();
        audio_play(play);
        if (!play) {
            playhead = NO_STEP;
        }
    } else if (in_span(REC_SPAN, column)) {
        recording = !recording;
    } else if (in_span(BPM_DOWN_SPAN, column)) {
        if (pattern->bpm > DRUM_BPM_MIN) {
            --pattern->bpm;
        }
    } else if (in_span(BPM_UP_SPAN, column)) {
        if (pattern->bpm < DRUM_BPM_MAX) {
            ++pattern->bpm;
        }
    } else if (in_span(BAR_SPAN, column) && pattern->bars > 1) {
        /* Follow, then each bar in turn, then follow again. */
        if (follow) {
            follow = false;
            view_bar = 0;
        } else if (view_bar + 1 < pattern->bars) {
            ++view_bar;
        } else {
            follow = true;
        }
    }
}

static void tap_grid(int x, int y)
{
    int track = y / FH - GRID_ROW;
    if (x < LABEL_WIDTH) {
        pattern->muted ^= (uint8_t)(1u << track);
        return;
    }
    int step = (x - LABEL_WIDTH) / STEP_WIDTH;
    if (step >= DRUM_STEPS) {
        step = DRUM_STEPS - 1;
    }
    pattern->steps[view_bar][track] ^= (uint16_t)(1u << step);
}

static void full_redraw(void)
{
    invalidate();
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    refresh();
}

static void play_pad(int x)
{
    int pad = x / PAD_WIDTH;
    if (pad >= DRUM_TRACKS) {
        pad = DRUM_TRACKS - 1;
    }
    audio_hit(pad, recording && audio_is_playing());
    atomic_fetch_or(&pads_hit, 1u << pad);
}

/* Runs in the touch task for every tap: a pad goes to the audio task at
   once instead of waiting for this task to finish a redraw. */
static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    if (!pads_live || point->y < PAD_TOP) {
        return false;
    }
    play_pad(point->x);
    return true;
}

static void flash_hit_pads(void)
{
    unsigned hit = atomic_exchange(&pads_hit, 0);
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(FLASH_MS);
    for (int pad = 0; pad < DRUM_TRACKS; ++pad) {
        if (hit & (1u << pad)) {
            flash_until[pad] = until;
        }
    }
}

/* The performance screen: fast multi-finger taps, pads played by on_tap. */
static void enter_pads(QueueHandle_t touch_queue)
{
    full_redraw();
    xQueueReset(touch_queue);
    atomic_store(&pads_hit, 0);
    pads_live = true;
    cyd_touch_set_fast(true);
}

void ui_run(QueueHandle_t touch_queue, drum_pattern_t *loop_pattern)
{
    pattern = loop_pattern;
    track_colors[DRUM_KICK] = cyd_rgb(255, 70, 40);
    track_colors[DRUM_SNARE] = cyd_rgb(255, 200, 0);
    track_colors[DRUM_CLAP] = cyd_rgb(255, 130, 0);
    track_colors[DRUM_CLOSED_HAT] = cyd_rgb(0, 210, 255);
    track_colors[DRUM_OPEN_HAT] = cyd_rgb(70, 130, 255);
    track_colors[DRUM_TOM] = cyd_rgb(60, 220, 90);
    track_colors[DRUM_RIM] = cyd_rgb(230, 90, 230);
    track_colors[DRUM_EXTRA] = cyd_rgb(220, 220, 200);
    playhead = NO_STEP;

    cyd_touch_set_tap_callback(on_tap, NULL);
    enter_pads(touch_queue);
    while (true) {
        if (output_check_lost()) {
            speaker_lost_notice = true;
        }
        cyd_touch_point_t point;
        TickType_t wait = pdMS_TO_TICKS(REFRESH_MS);
        /* Take every tap waiting before drawing, so pads answer first. */
        while (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            wait = 0;
            if (point.y >= PAD_TOP) {
                play_pad(point.x); /* only if it came before pads_live */
            } else if (point.y >= GRID_ROW * FH) {
                tap_grid(point.x, point.y);
            } else if (in_span(MENU_SPAN, point.x / FW)) {
                run_menu(touch_queue);
                enter_pads(touch_queue);
                break;
            } else {
                tap_header(point.x / FW);
            }
        }
        flash_hit_pads();
        update_playhead();
        refresh();
    }
}
