"""Makes Drum&Key's two screens from DrumLoop's and Keyboard's ui.c.

main/drums_ui.c and main/keys_ui.c are those apps' screens with a few
changes: the shared header (dk_ui.h) with the KEYS / DRUM switch button,
the shared transport and tempo (dk_audio.h), and an entry point that
returns when the switch is tapped. This script makes those changes, so
when either app's ui.c changes, Drum&Key's screens follow:

    python apps/drumkey/tools/make_screens.py           # rewrite the screens
    python apps/drumkey/tools/make_screens.py --check   # exit 1 if stale

Each change must find its text exactly once, or the script stops and
names the one that no longer matches; then update that change here to
suit the new ui.c. CI runs --check.
"""

import os
import re
import sys

apps = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
check = "--check" in sys.argv[1:]
stale = []


def load(p):
    with open(os.path.join(apps, p), encoding="utf-8") as f:
        return f.read()


def save(p, s):
    path = os.path.join(apps, p)
    if check:
        if not os.path.isfile(path) or load(p) != s:
            stale.append(p)
        return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)


class Doc:
    def __init__(self, s):
        self.s = s

    def rep(self, a, b, n=1):
        assert self.s.count(a) == n, (a[:80], self.s.count(a))
        self.s = self.s.replace(a, b)

    def sub(self, pattern, b, n=None):
        new, count = re.subn(pattern, b, self.s)
        assert count > 0 and (n is None or count == n), (pattern, count)
        self.s = new


SPAN_TYPEDEF = '''typedef struct {
    int column;
    int width;
} span_t;

'''

# ---- Drums screen ----------------------------------------------------------
d = Doc(load('drumloop/main/ui.c'))
d.rep('''/* Performance screen, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 140 +, bar page, MENU''',
      '''/* Drum&Key's drums screen, adapted from DrumLoop's ui.c, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 140 +, bar page, KEYS
                 (to the keys screen), MENU''')
d.rep('#include "ui.h"\n', '#include "dk_ui.h"\n')
d.rep('#include "audio.h"\n', '#include "dk_audio.h"\n')
d.rep(SPAN_TYPEDEF, '')
d.rep('''static const span_t PLAY_SPAN = {0, 4};
static const span_t REC_SPAN = {5, 3};
static const span_t BPM_DOWN_SPAN = {9, 2};
static const span_t BPM_SPAN = {11, 3};
static const span_t BPM_UP_SPAN = {14, 2};
static const span_t BAR_SPAN = {17, 5};
static const span_t MENU_SPAN = {22, 4};''', '''static const span_t PLAY_SPAN = DK_PLAY_SPAN;
static const span_t REC_SPAN = DK_REC_SPAN;
static const span_t BPM_DOWN_SPAN = DK_BPM_DOWN_SPAN;
static const span_t BPM_SPAN = DK_BPM_SPAN;
static const span_t BPM_UP_SPAN = DK_BPM_UP_SPAN;
static const span_t BAR_SPAN = DK_SCREEN_SPAN;
static const span_t SWITCH_SPAN = DK_SWITCH_SPAN;
static const span_t MENU_SPAN = DK_MENU_SPAN;''')
d.sub(r'\baudio_is_playing\(\)', 'dk_is_playing()')
d.sub(r'\baudio_play\(', 'dk_play(')
d.sub(r'\baudio_hit\(', 'drums_hit(')
d.sub(r'\baudio_next_heard_step\(', 'drums_next_heard_step(')
d.rep('''    char bar[8];
    snprintf(bar, sizeof(bar), "%d/%u", view_bar + 1, pattern->bars);''', '''    char bar[8];
    snprintf(bar, sizeof(bar), "%d/%u", view_bar + 1, pattern->bars);
    if (strlen(bar) > (size_t)BAR_SPAN.width) {
        snprintf(bar, sizeof(bar), "B%d", view_bar + 1); /* 10 bars and up */
    }''')
d.rep('''    put_span(text, fg, bg, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
''', '''    put_span(text, fg, bg, SWITCH_SPAN, "KEYS", CYD_COLOR_WHITE, DK_KEYS_COLOR);
    put_span(text, fg, bg, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
''')
d.rep('''    ESP_ERROR_CHECK(cyd_draw_cells(0, text, fg, bg));
    memcpy(drawn_header, text, sizeof(drawn_header));''', '''    ESP_ERROR_CHECK(cyd_draw_cells(0, text, fg, bg));
    dk_header_gaps();
    memcpy(drawn_header, text, sizeof(drawn_header));''')
d.rep('" DRUMLOOP SETTINGS"', '" DRUM&KEY: DRUM SETTINGS"')
d.rep('''void ui_run(QueueHandle_t touch_queue, drum_pattern_t *loop_pattern)
{
    pattern = loop_pattern;
    saved_bpm = changed_bpm = pattern->bpm;''', '''static void setup(void)
{
    pattern = dk_pattern();
    saved_bpm = changed_bpm = pattern->bpm;''')
d.rep('''    playhead = NO_STEP;

    cyd_touch_set_tap_callback(on_tap, NULL);
    enter_pads(touch_queue);''', '''    playhead = NO_STEP;
}

/* Runs the drums screen until its KEYS button is tapped. */
void drums_screen(QueueHandle_t touch_queue)
{
    static bool started;
    if (!started) {
        started = true;
        setup();
    }
    /* The keys screen had the touch callbacks. */
    cyd_touch_set_fingers_callback(NULL, NULL);
    cyd_touch_set_tap_callback(on_tap, NULL);
    enter_pads(touch_queue);''')
d.rep('''            } else if (in_span(MENU_SPAN, point.x / FW)) {
                run_menu(touch_queue);
                enter_pads(touch_queue);
                break;
            } else {''', '''            } else if (in_span(MENU_SPAN, point.x / FW)) {
                run_menu(touch_queue);
                enter_pads(touch_queue);
                break;
            } else if (in_span(SWITCH_SPAN, point.x / FW)) {
                pads_live = false;
                cyd_touch_set_tap_callback(NULL, NULL);
                return;
            } else {''')
save('drumkey/main/drums_ui.c', d.s)

# ---- Keys screen -----------------------------------------------------------
k = Doc(load('keyboard/main/ui.c'))
k.rep('''/* Keyboard screen, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 120 +, MET, MENU''',
      '''/* Drum&Key's keys screen, adapted from Keyboard's ui.c, 800x480:

     row 0       transport: PLAY/STOP, REC, BPM - 120 +, MET, DRUM (to the
                 drums screen), MENU''')
k.rep('#include "ui.h"\n', '#include "dk_ui.h"\n')
k.rep('#include "audio.h"\n', '#include "dk_audio.h"\n')
k.rep(SPAN_TYPEDEF, '')
k.rep('''static const span_t PLAY_SPAN = {0, 4};
static const span_t REC_SPAN = {5, 3};
static const span_t BPM_DOWN_SPAN = {9, 2};
static const span_t BPM_SPAN = {11, 3};
static const span_t BPM_UP_SPAN = {14, 2};
static const span_t MET_SPAN = {17, 4};
static const span_t MENU_SPAN = {22, 4};''', '''static const span_t PLAY_SPAN = DK_PLAY_SPAN;
static const span_t REC_SPAN = DK_REC_SPAN;
static const span_t BPM_DOWN_SPAN = DK_BPM_DOWN_SPAN;
static const span_t BPM_SPAN = DK_BPM_SPAN;
static const span_t BPM_UP_SPAN = DK_BPM_UP_SPAN;
static const span_t MET_SPAN = DK_SCREEN_SPAN;
static const span_t SWITCH_SPAN = DK_SWITCH_SPAN;
static const span_t MENU_SPAN = DK_MENU_SPAN;''')
# The tempo is the shared one.
k.rep('''    audio_settings.bpm = (uint16_t)bpm;''', '''    dk_set_tempo(bpm);''')
k.sub(r'\baudio_settings\.bpm\b', 'dk_tempo()')
k.sub(r'\baudio_settings\b', 'keys_settings')
for a, b in [('audio_note_on', 'keys_note_on'), ('audio_note_off', 'keys_note_off'),
             ('audio_sustain', 'keys_sustain'), ('audio_all_notes_off', 'keys_all_notes_off'),
             ('audio_undo', 'keys_undo'), ('audio_clear', 'keys_clear'),
             ('audio_cycle_bars', 'keys_cycle_bars'), ('audio_next_heard_note', 'keys_next_heard_note')]:
    k.sub(r'\b' + a + r'\(', b + '(')
k.rep('''    if (in_span(PLAY_SPAN, column)) {
        if (state->playing) {
            audio_stop();
        } else {
            audio_play();
        }
    } else if (in_span(REC_SPAN, column)) {
        audio_record(!state->recording);''', '''    if (in_span(PLAY_SPAN, column)) {
        dk_play(!dk_is_playing()); /* the drums too */
    } else if (in_span(REC_SPAN, column)) {
        /* From stop, both loops start and recording begins at bar 1. */
        if (!state->recording && !dk_is_playing()) {
            dk_play(true);
        }
        keys_record(!state->recording);''')
k.rep('''    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    draw_row(0, &row);''', '''    put_span(&row, SWITCH_SPAN, "DRUM", CYD_COLOR_WHITE, DK_DRUMS_COLOR);
    put_span(&row, MENU_SPAN, "MENU", CYD_COLOR_WHITE,
             speaker_lost_notice ? CYD_COLOR_RED : CYD_COLOR_BUTTON);
    draw_row(0, &row);''')
k.rep('''    ESP_ERROR_CHECK(cyd_draw_cells(index, row->text, row->fg, row->bg));
    *drawn = *row;''', '''    ESP_ERROR_CHECK(cyd_draw_cells(index, row->text, row->fg, row->bg));
    if (index == 0) {
        dk_header_gaps();
    }
    *drawn = *row;''')
# No count-in: the drums keep time, and REC from stop starts both loops.
k.rep('''static void draw_count_in_row(void)
{
    char line[COLUMNS + 1];
    snprintf(line, sizeof(line), " COUNT-IN     %s", keys_settings.count_in ? "1 BAR" : "OFF");
    draw_button_row(MENU_COUNT_IN, line);
}''', '''static void draw_count_in_row(void)
{
    ESP_ERROR_CHECK(cyd_text_row(MENU_COUNT_IN, CYD_COLOR_GRAY, CYD_COLOR_BLACK,
                                 " COUNT-IN     OFF: the drums keep time"));
}''')
k.rep('''        case MENU_COUNT_IN:
            keys_settings.count_in = !keys_settings.count_in;
            draw_count_in_row();
            break;
''', '')
k.rep('" KEYBOARD SETTINGS"', '" DRUM&KEY: KEYS SETTINGS"')
k.rep('''void ui_run(QueueHandle_t touch_queue)
{''', '''static void setup(void)
{''')
k.rep('''    saved_bpm = changed_bpm = dk_tempo();

    cyd_touch_set_tap_callback(on_tap, NULL);
    cyd_touch_set_fingers_callback(on_fingers, NULL);
    enter_keys(touch_queue);''', '''    saved_bpm = changed_bpm = dk_tempo();
}

/* Runs the keys screen until its DRUM button is tapped. */
void keys_screen(QueueHandle_t touch_queue)
{
    static bool started;
    if (!started) {
        started = true;
        setup();
    }
    cyd_touch_set_tap_callback(on_tap, NULL);
    cyd_touch_set_fingers_callback(on_fingers, NULL);
    enter_keys(touch_queue);''')
k.rep('''            if (row == 0 && in_span(MENU_SPAN, column)) {
                leave_keys();
                run_menu(touch_queue);
                enter_keys(touch_queue);
                break;
            }''', '''            if (row == 0 && in_span(MENU_SPAN, column)) {
                leave_keys();
                run_menu(touch_queue);
                enter_keys(touch_queue);
                break;
            }
            if (row == 0 && in_span(SWITCH_SPAN, column)) {
                leave_keys();
                cyd_touch_set_fingers_callback(NULL, NULL);
                cyd_touch_set_tap_callback(NULL, NULL);
                return;
            }''')
save('drumkey/main/keys_ui.c', k.s)
if stale:
    print("Out of date; run python apps/drumkey/tools/make_screens.py: " + ", ".join(stale))
    sys.exit(1)
print("Drum&Key's screens are up to date" if check else "Wrote Drum&Key's screens")
