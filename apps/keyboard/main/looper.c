#include "looper.h"

#include <math.h>
#include <string.h>

#define CHUNK 32 /* frames between transport updates: 0.7 ms */
#define MAX_ACTIVE 32
#define MAX_PENDING 16
#define MIN_LENGTH (LOOP_TICKS_PER_BEAT / 16) /* a 64th note */
#define NO_SKIP 0xFFFF
#define DEFAULT_BARS 2

typedef struct {
    uint32_t start;  /* ticks from the start of the loop */
    uint32_t length; /* ticks */
    uint16_t take;
    uint16_t skip_pass; /* a pass not to play it in (see looper_live_on) */
    uint8_t note;
    uint8_t instrument;
} loop_note_t;

/* A loop note sounding now, and when it ends. */
typedef struct {
    double end; /* in abs_ticks */
    uint8_t note;
    uint8_t instrument;
} active_t;

/* A live note being recorded: its start is known, its length not yet. */
typedef struct {
    bool used;
    bool released; /* key up, held by the pedal */
    uint8_t note;
    uint8_t instrument;
    uint16_t skip_pass;
    uint32_t start;
    double start_abs;
} pending_t;

static loop_note_t notes[LOOP_MAX_NOTES];
static int note_count;
static active_t active[MAX_ACTIVE];
static int active_count;
static pending_t pending[MAX_PENDING];
static looper_note_cb_t note_callback;

static bool playing;
static bool recording;
static bool sustain_down;
static uint8_t bars = DEFAULT_BARS;
static uint16_t take;  /* the current or last take */
static uint16_t pass;  /* times round the loop since play */
static double pos;     /* render position in ticks; negative in the count-in */
static double abs_ticks; /* ticks since play, for ending notes */
static double start_pos; /* where play started: 0, or minus a bar to count in */
static double ticks_per_frame = 120.0 * LOOP_TICKS_PER_BEAT / (60.0 * SYNTH_RATE);
static uint32_t chunk_frame; /* stream frame of the chunk being rendered */
static quantize_t quantize = QUANTIZE_OFF;
static bool count_in_enabled = true;
static looper_state_t snapshot;

static double loop_length(void)
{
    return (double)bars * LOOP_TICKS_PER_BAR;
}

static double wrap(double ticks)
{
    double length = loop_length();
    ticks = fmod(ticks, length);
    if (ticks < 0) {
        ticks += length;
    }
    return ticks >= length ? 0.0 : ticks;
}

static double snap(double ticks)
{
    double grid = quantize == QUANTIZE_8TH    ? LOOP_TICKS_PER_BEAT / 2
                  : quantize == QUANTIZE_16TH ? LOOP_TICKS_PER_BEAT / 4
                                              : 0;
    return grid > 0 ? floor(ticks / grid + 0.5) * grid : ticks;
}

static void add_note(const loop_note_t *note)
{
    if (note_count < LOOP_MAX_NOTES) {
        notes[note_count++] = *note;
    }
}

static void finish_pending(pending_t *p)
{
    double length = abs_ticks - p->start_abs;
    double limit = loop_length();
    length = length < MIN_LENGTH ? MIN_LENGTH : length > limit ? limit : length;
    loop_note_t note = {
        .start = p->start,
        .length = (uint32_t)length,
        .take = take,
        .skip_pass = p->skip_pass,
        .note = p->note,
        .instrument = p->instrument,
    };
    add_note(&note);
    p->used = false;
}

static void finish_all_pending(void)
{
    for (int i = 0; i < MAX_PENDING; ++i) {
        if (pending[i].used) {
            finish_pending(&pending[i]);
        }
    }
}

static void end_active(int index)
{
    active_t *a = &active[index];
    synth_note_off(a->note, a->instrument, SOURCE_LOOP);
    if (note_callback != NULL) {
        note_callback(chunk_frame, a->note, false);
    }
    active[index] = active[--active_count];
}

static void end_all_active(void)
{
    while (active_count > 0) {
        end_active(active_count - 1);
    }
    synth_release_all(SOURCE_LOOP);
}

void looper_init(looper_note_cb_t on_note)
{
    note_callback = on_note;
    note_count = 0;
    active_count = 0;
    memset(pending, 0, sizeof(pending));
    bars = DEFAULT_BARS;
}

/* ---- Live keys ------------------------------------------------------------ */

void looper_live_on(int note, int instrument, uint32_t latency_frames)
{
    synth_note_on(note, instrument, SOURCE_LIVE);
    if (!recording || !playing) {
        return;
    }
    /* The player was hearing the loop latency_frames behind the render. */
    double heard = pos - latency_frames * ticks_per_frame;
    double start = snap(heard);
    /* Snapped ahead of the render position (with little latency), the loop
       would play the note again a moment after the live one: skip it on
       the pass that reaches it first. */
    uint16_t skip = NO_SKIP;
    if (start > pos && start >= 0) {
        skip = start < loop_length() ? pass : (uint16_t)(pass + 1);
    }
    pending_t *slot = NULL;
    for (int i = 0; i < MAX_PENDING; ++i) {
        pending_t *p = &pending[i];
        if (p->used && p->note == note && p->instrument == instrument) {
            finish_pending(p); /* the same key again while the pedal held it */
        }
        if (!p->used && slot == NULL) {
            slot = p;
        }
    }
    if (slot == NULL) {
        return;
    }
    *slot = (pending_t){
        .used = true,
        .note = (uint8_t)note,
        .instrument = (uint8_t)instrument,
        .skip_pass = skip,
        .start = (uint32_t)wrap(start),
        .start_abs = abs_ticks,
    };
}

void looper_live_off(int note, int instrument)
{
    synth_note_off(note, instrument, SOURCE_LIVE);
    for (int i = 0; i < MAX_PENDING; ++i) {
        pending_t *p = &pending[i];
        if (p->used && !p->released && p->note == note && p->instrument == instrument) {
            if (sustain_down) {
                p->released = true;
            } else {
                finish_pending(p);
            }
        }
    }
}

void looper_sustain(bool down)
{
    sustain_down = down;
    synth_sustain(down);
    if (down) {
        return;
    }
    for (int i = 0; i < MAX_PENDING; ++i) {
        if (pending[i].used && pending[i].released) {
            finish_pending(&pending[i]);
        }
    }
}

void looper_live_all_off(void)
{
    sustain_down = false;
    synth_sustain(false);
    finish_all_pending();
    synth_release_all(SOURCE_LIVE);
}

/* ---- Transport ------------------------------------------------------------ */

static void start_from(double ticks)
{
    end_all_active();
    playing = true;
    pos = ticks;
    start_pos = ticks;
    abs_ticks = 0;
    pass = 0;
}

void looper_play(void)
{
    if (!playing) {
        start_from(0);
    }
}

void looper_stop(void)
{
    finish_all_pending();
    recording = false;
    playing = false;
    end_all_active();
}

void looper_record(bool on)
{
    if (on == recording) {
        return;
    }
    if (!on) {
        finish_all_pending();
        recording = false;
        return;
    }
    ++take;
    if (!playing) {
        start_from(count_in_enabled ? -(double)LOOP_TICKS_PER_BAR : 0);
    }
    recording = true;
}

bool looper_undo(void)
{
    if (recording) {
        looper_record(false);
    }
    int last = -1;
    for (int i = 0; i < note_count; ++i) {
        if (notes[i].take > last) {
            last = notes[i].take;
        }
    }
    if (last < 0) {
        return false;
    }
    int kept = 0;
    for (int i = 0; i < note_count; ++i) {
        if (notes[i].take != last) {
            notes[kept++] = notes[i];
        }
    }
    note_count = kept;
    return true;
}

void looper_clear(void)
{
    memset(pending, 0, sizeof(pending));
    note_count = 0;
    end_all_active();
}

void looper_cycle_bars(void)
{
    if (bars >= LOOP_MAX_BARS) {
        bars = 1;
        double length = loop_length();
        int kept = 0;
        for (int i = 0; i < note_count; ++i) {
            if (notes[i].start < length) {
                notes[kept++] = notes[i];
            }
        }
        note_count = kept;
        for (int i = 0; i < MAX_PENDING; ++i) {
            pending[i].start = (uint32_t)wrap(pending[i].start);
        }
        if (pos >= length) {
            pos = wrap(pos);
        }
        return;
    }
    uint32_t old_length = (uint32_t)loop_length();
    int count = note_count;
    for (int i = 0; i < count; ++i) {
        if (notes[i].start < old_length) {
            loop_note_t copy = notes[i];
            copy.start += old_length;
            copy.skip_pass = NO_SKIP;
            add_note(&copy);
        }
    }
    bars = (uint8_t)(bars * 2);
}

/* ---- Rendering ------------------------------------------------------------ */

/* Starts the loop notes whose start lies in [from, to) of the loop, on
   pass p. abs_at_from is abs_ticks at loop position from. */
static void start_notes(double from, double to, uint16_t p, double abs_at_from)
{
    for (int i = 0; i < note_count; ++i) {
        const loop_note_t *note = &notes[i];
        if (note->start < from || note->start >= to || note->skip_pass == p) {
            continue;
        }
        if (active_count == MAX_ACTIVE) {
            /* End the one closest to ending anyway. */
            int soonest = 0;
            for (int j = 1; j < active_count; ++j) {
                if (active[j].end < active[soonest].end) {
                    soonest = j;
                }
            }
            end_active(soonest);
        }
        synth_note_on(note->note, note->instrument, SOURCE_LOOP);
        active[active_count++] = (active_t){
            .end = abs_at_from + (note->start - from) + note->length,
            .note = note->note,
            .instrument = note->instrument,
        };
        if (note_callback != NULL) {
            note_callback(chunk_frame, note->note, true);
        }
    }
}

static void advance(size_t frames, const looper_settings_t *settings)
{
    double span = frames * ticks_per_frame;
    double start = pos;
    double end = pos + span;
    double length = loop_length();

    for (int i = active_count - 1; i >= 0; --i) {
        if (active[i].end <= abs_ticks + span) {
            end_active(i);
        }
    }

    /* Clicks on each beat: always in the count-in, else if the metronome
       is on. The first beat of a bar is accented. */
    for (double beat = ceil(start / LOOP_TICKS_PER_BEAT); beat * LOOP_TICKS_PER_BEAT < end; beat += 1) {
        if (beat < 0 || settings->metronome) {
            int in_bar = ((int)beat % LOOP_BEATS_PER_BAR + LOOP_BEATS_PER_BAR) % LOOP_BEATS_PER_BAR;
            synth_click(in_bar == 0);
        }
    }

    if (end > 0) {
        double from = start < 0 ? 0 : start;
        double abs_at_from = abs_ticks + (from - start);
        if (end <= length) {
            start_notes(from, end, pass, abs_at_from);
        } else {
            start_notes(from, length, pass, abs_at_from);
            start_notes(0, end - length, (uint16_t)(pass + 1), abs_at_from + (length - from));
        }
    }

    pos = end;
    abs_ticks += span;
    if (pos >= length) {
        pos -= length;
        ++pass;
    }
}

void looper_render(int16_t *pcm, size_t frames, uint32_t first_frame,
                   const looper_settings_t *settings)
{
    ticks_per_frame = settings->bpm * (double)LOOP_TICKS_PER_BEAT / (60.0 * SYNTH_RATE);
    quantize = settings->quantize;
    count_in_enabled = settings->count_in;
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done > CHUNK ? CHUNK : frames - done;
        chunk_frame = first_frame + (uint32_t)done;
        if (playing) {
            advance(n, settings);
        }
        synth_render(pcm + done, n);
        done += n;
    }
    snapshot = (looper_state_t){
        .playing = playing,
        .recording = recording,
        .counting_in = playing && pos < 0,
        .bars = bars,
        .notes = (uint16_t)note_count,
        .takes = take,
        .position = playing ? (int32_t)pos : 0,
        .start = (int32_t)start_pos,
        .elapsed = playing ? (uint32_t)abs_ticks : 0,
    };
}

void looper_get_state(looper_state_t *state)
{
    *state = snapshot;
}
