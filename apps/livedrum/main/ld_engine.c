#include "ld_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "music_out.h"

#define HIT_QUEUE_LENGTH 32
/* Loop hits and beats on their way to the speaker: 2 s of a busy loop. */
#define HEARD_QUEUE_LENGTH 256
#define MIX_CHUNK 256
#define MASTER_GAIN 0.8f
/* A retriggered pad fades out over 5 ms instead of stopping dead; a choked
   one over 10 ms. */
#define RETRIGGER_FADE (LD_RATE / 200)
#define CHOKE_FADE (LD_RATE / 100)
#define UNITY_STEP 65536u
/* A hit recorded this close ahead of the render would play again at once
   (the quantize moved it forward): it is let pass once. */
#define SKIP_TICKS (LD_TICKS_PER_BEAT / 4)
/* A hit up to a sixteenth before bar 1, as heard, belongs on bar 1. */
#define EARLY_TICKS (LD_TICKS_PER_BEAT / 4)
#define CLICK_FRAMES (LD_RATE / 40)
#define CLICK_GAIN 0.45f
#define CALL_WARN_MS 2000

static const char *TAG = "ld_engine";

typedef struct {
    const int16_t *pcm; /* NULL when idle */
    uint32_t frames;
    uint32_t pos;  /* frame */
    uint32_t frac; /* and 1/65536ths of one */
    uint32_t step; /* frames per output frame, 16.16 */
    float gain;    /* per sample value */
    uint16_t fade_left; /* fading out over fade_len frames; 0 when not */
    uint16_t fade_len;
} voice_t;

/* A pad's hit, and the one before it fading out under it. */
typedef struct {
    voice_t main;
    voice_t tail;
} pad_voice_t;

typedef void (*call_fn_t)(void *argument);

volatile bool ld_metronome;
volatile uint8_t ld_quantize = LD_QUANTIZE_16;

static ld_pad_settings_t settings[LD_PADS];
static ld_sample_t samples[LD_PADS]; /* written only in the audio task */
static pad_voice_t voices[LD_PADS];
static voice_t click_voice;
static int16_t *click_high; /* the bar's first beat */
static int16_t *click_low;

static QueueHandle_t hit_queue;
static QueueHandle_t heard_queue;

/* The loop, kept sorted by tick. Only the audio task changes it. */
static ld_loop_hit_t *hits;
static volatile int hit_count;
static volatile int take_count;
static volatile int bars = 1;
static volatile bool recording;
static uint8_t current_take;

/* Transport, in the audio task. */
static volatile bool playing;
static double cursor;    /* ticks into the loop; negative in the count-in */
static double played;    /* ticks since bar 1 first started, for the count-in */
static int next_hit;     /* the next hit to play in this pass */
static int32_t next_beat; /* tick of the next beat */
static uint16_t render_bpm = 120;

/* Calls into the audio task (see call). */
static SemaphoreHandle_t call_mutex;
static SemaphoreHandle_t call_done;
static call_fn_t pending_call;
static void *pending_argument;
static volatile bool rendering;

/* ---- Calls into the audio task --------------------------------------------- */

/* Runs fn in the audio task at the start of its next block and waits for
   it, so the loop and the pads' sounds are only ever changed there. Before
   the audio task runs, fn runs here. Once it runs, fn never runs anywhere
   else, even if the task is held up (a slow AirPlay send): a sample must
   not be freed while a voice may still read it. */
static void call(call_fn_t fn, void *argument)
{
    if (!rendering) {
        fn(argument);
        return;
    }
    xSemaphoreTake(call_mutex, portMAX_DELAY);
    pending_argument = argument;
    __atomic_store_n(&pending_call, fn, __ATOMIC_RELEASE);
    while (xSemaphoreTake(call_done, pdMS_TO_TICKS(CALL_WARN_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Waiting for the audio task");
    }
    xSemaphoreGive(call_mutex);
}

static void run_pending_call(void)
{
    call_fn_t fn = __atomic_exchange_n(&pending_call, NULL, __ATOMIC_ACQ_REL);
    if (fn != NULL) {
        fn(pending_argument);
        xSemaphoreGive(call_done);
    }
}

/* ---- Voices ------------------------------------------------------------------ */

static uint32_t pitch_step(int semitones)
{
    if (semitones == 0) {
        return UNITY_STEP;
    }
    return (uint32_t)lrintf(UNITY_STEP * powf(2.0f, semitones / 12.0f));
}

/* Moves a sounding hit to the tail slot, to fade out over frames. */
static void fade_out(pad_voice_t *voice, uint16_t frames)
{
    if (voice->main.pcm != NULL) {
        voice->tail = voice->main;
        voice->tail.fade_left = frames;
        voice->tail.fade_len = frames;
    }
    voice->main.pcm = NULL;
}

static void trigger(int pad)
{
    const ld_sample_t *sample = &samples[pad];
    if (sample->pcm == NULL || sample->frames == 0) {
        return;
    }
    uint8_t choke = settings[pad].choke;
    if (choke != 0) {
        for (int other = 0; other < LD_PADS; ++other) {
            if (other != pad && settings[other].choke == choke) {
                fade_out(&voices[other], CHOKE_FADE);
            }
        }
    }
    pad_voice_t *voice = &voices[pad];
    fade_out(voice, RETRIGGER_FADE);
    int level = settings[pad].level;
    voice->main = (voice_t){
        .pcm = sample->pcm,
        .frames = sample->frames,
        .step = pitch_step(settings[pad].pitch),
        .gain = sample->gain * (level > LD_LEVEL_MAX ? LD_LEVEL_MAX : level) /
                (LD_LEVEL_MAX * 32768.0f),
    };
}

/* Adds a voice into acc, pitched by its step, fading if it is a tail. */
static void add_voice(voice_t *v, float *acc, size_t frames)
{
    if (v->pcm == NULL) {
        return;
    }
    const int16_t *pcm = v->pcm;
    float gain = v->gain;
    if (v->step == UNITY_STEP && v->fade_left == 0) {
        size_t n = v->frames - v->pos;
        if (n > frames) {
            n = frames;
        }
        for (size_t i = 0; i < n; ++i) {
            acc[i] += pcm[v->pos + i] * gain;
        }
        v->pos += (uint32_t)n;
        if (v->pos >= v->frames) {
            v->pcm = NULL;
        }
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        if (v->pos >= v->frames) {
            v->pcm = NULL;
            return;
        }
        float a = pcm[v->pos];
        float b = v->pos + 1 < v->frames ? pcm[v->pos + 1] : 0.0f;
        float s = a + (b - a) * (v->frac * (1.0f / UNITY_STEP));
        if (v->fade_left > 0) {
            s *= (float)v->fade_left / v->fade_len;
            if (--v->fade_left == 0) {
                v->pcm = NULL;
                acc[i] += s * gain;
                return;
            }
        }
        acc[i] += s * gain;
        v->frac += v->step;
        v->pos += v->frac >> 16;
        v->frac &= 0xFFFF;
    }
}

/* Soft clipper: linear for quiet signals, rounding off toward +-1. */
static float soft_clip(float x)
{
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

static void mix(int16_t *out, size_t frames)
{
    float acc[MIX_CHUNK];
    memset(acc, 0, frames * sizeof(float));
    for (int pad = 0; pad < LD_PADS; ++pad) {
        add_voice(&voices[pad].main, acc, frames);
        add_voice(&voices[pad].tail, acc, frames);
    }
    add_voice(&click_voice, acc, frames);
    for (size_t i = 0; i < frames; ++i) {
        out[i] = (int16_t)lrintf(soft_clip(acc[i] * MASTER_GAIN) * 32767.0f);
    }
}

static void silence(void)
{
    for (int pad = 0; pad < LD_PADS; ++pad) {
        voices[pad].main.pcm = NULL;
        voices[pad].tail.pcm = NULL;
    }
    click_voice.pcm = NULL;
}

static void click(bool high)
{
    click_voice = (voice_t){
        .pcm = high ? click_high : click_low,
        .frames = CLICK_FRAMES,
        .step = UNITY_STEP,
        .gain = CLICK_GAIN / 32768.0f,
    };
}

/* ---- The loop, in the audio task --------------------------------------------- */

static int loop_ticks(void)
{
    return bars * LD_TICKS_PER_BAR;
}

static double ticks_per_frame(uint16_t bpm)
{
    if (bpm < MUSIC_TEMPO_MIN) {
        bpm = MUSIC_TEMPO_MIN;
    }
    return (double)bpm * LD_TICKS_PER_BEAT / (60.0 * LD_RATE);
}

static void count_takes(void)
{
    uint32_t seen[8] = {0};
    int takes = 0;
    for (int h = 0; h < hit_count; ++h) {
        uint8_t take = hits[h].take;
        if (!(seen[take / 32] & (1u << (take % 32)))) {
            seen[take / 32] |= 1u << (take % 32);
            ++takes;
        }
    }
    take_count = takes;
}

/* After the loop changed: the next hit to play is the first after the
   cursor (everything at or before it has played), and the takes are
   counted again. */
static void reposition(void)
{
    int i = 0;
    if (cursor >= 0) {
        while (i < hit_count && hits[i].tick <= cursor) {
            ++i;
        }
    }
    next_hit = i;
    count_takes();
}

static uint8_t highest_take(void)
{
    uint8_t highest = 0;
    for (int h = 0; h < hit_count; ++h) {
        if (hits[h].take > highest) {
            highest = hits[h].take;
        }
    }
    return highest;
}

/* Take numbers run out after 255: numbers them 1, 2, 3... again, in order. */
static void renumber_takes(void)
{
    uint8_t map[256] = {0};
    for (int h = 0; h < hit_count; ++h) {
        map[hits[h].take] = 1;
    }
    uint8_t next = 1;
    for (int take = 1; take < 256; ++take) {
        if (map[take]) {
            map[take] = next++;
        }
    }
    for (int h = 0; h < hit_count; ++h) {
        hits[h].take = map[hits[h].take];
    }
}

static void begin_take(void)
{
    if (recording) {
        return;
    }
    if (highest_take() >= 250) {
        renumber_takes();
    }
    if (highest_take() == 255) {
        /* 255 takes in the loop: the two oldest become one. */
        for (int h = 0; h < hit_count; ++h) {
            if (hits[h].take > 1) {
                --hits[h].take;
            }
        }
    }
    current_take = (uint8_t)(highest_take() + 1);
    recording = true;
}

static void end_take(void)
{
    recording = false;
}

static int quantize_ticks(void)
{
    switch (ld_quantize) {
    case LD_QUANTIZE_8:
        return LD_TICKS_PER_BEAT / 2;
    case LD_QUANTIZE_16:
        return LD_TICKS_PER_BEAT / 4;
    case LD_QUANTIZE_32:
        return LD_TICKS_PER_BEAT / 8;
    case LD_QUANTIZE_8T:
        return LD_TICKS_PER_BEAT / 3;
    case LD_QUANTIZE_16T:
        return LD_TICKS_PER_BEAT / 6;
    default:
        return 1;
    }
}

/* Writes a live hit into the loop where the player heard it. */
static void record_hit(int pad)
{
    if (!playing || !recording) {
        return;
    }
    double latency = music_out_latency_frames() * ticks_per_frame(render_bpm);
    int q = quantize_ticks();
    int total = loop_ticks();
    long tick;
    if (played - latency < 0) {
        /* Heard in the count-in (or before the loop was heard to start). */
        if (played - latency < -EARLY_TICKS) {
            return;
        }
        tick = 0;
    } else {
        tick = lround((cursor - latency) / q) * q;
        tick %= total;
        if (tick < 0) {
            tick += total;
        }
    }
    if (hit_count >= LD_MAX_HITS) {
        return;
    }
    int at = 0;
    while (at < hit_count && hits[at].tick <= tick) {
        if (hits[at].tick == tick && hits[at].pad == pad) {
            return; /* already there */
        }
        ++at;
    }
    memmove(&hits[at + 1], &hits[at], (size_t)(hit_count - at) * sizeof(hits[0]));
    /* A hit placed before next_hit has had its turn in this pass (it was
       just played live); one from next_hit on plays later in the pass.
       Either way, how far ahead of the render it next plays (below 0 when
       it is due now): */
    bool passed = at < next_hit;
    double ahead = (double)tick - cursor + (passed ? total : 0);
    hits[at] = (ld_loop_hit_t){
        .tick = (uint16_t)tick,
        .pad = (uint8_t)pad,
        .take = current_take,
        .skip = ahead < SKIP_TICKS,
    };
    ++hit_count;
    if (passed) {
        ++next_hit;
    }
    count_takes();
}

static void post_heard(uint32_t frame, ld_heard_type_t type, int pad, int beat)
{
    ld_heard_t heard = {
        .frame = frame,
        .type = (uint8_t)type,
        .pad = (uint8_t)pad,
        .beat = (int16_t)beat,
    };
    xQueueSend(heard_queue, &heard, 0);
}

/* Plays what is due at the cursor: the loop wrapping, beats and hits. */
static void play_due(uint32_t frame)
{
    int total = loop_ticks();
    while (true) {
        if (cursor >= total) {
            cursor -= total;
            next_beat -= total;
            next_hit = 0;
        } else if (next_beat <= cursor) {
            int beat = next_beat / LD_TICKS_PER_BEAT;
            if (next_beat < 0 || ld_metronome) {
                click(next_beat % LD_TICKS_PER_BAR == 0);
            }
            post_heard(frame, LD_HEARD_BEAT, 0, beat);
            next_beat += LD_TICKS_PER_BEAT;
        } else if (cursor >= 0 && next_hit < hit_count && hits[next_hit].tick <= cursor) {
            ld_loop_hit_t *hit = &hits[next_hit++];
            if (hit->skip) {
                hit->skip = 0;
            } else {
                trigger(hit->pad);
                post_heard(frame, LD_HEARD_PAD, hit->pad, 0);
            }
        } else {
            break;
        }
    }
}

/* Frames until the next thing play_due has to do, at least 1. */
static size_t frames_to_next(double per_frame)
{
    double target = next_beat;
    if (cursor >= 0) {
        if (next_hit < hit_count && hits[next_hit].tick < target) {
            target = hits[next_hit].tick;
        }
        if (loop_ticks() < target) {
            target = loop_ticks();
        }
    }
    double frames = ceil((target - cursor) / per_frame);
    return frames < 1.0 ? 1 : frames > MIX_CHUNK ? MIX_CHUNK : (size_t)frames;
}

void ld_engine_start(int count_in)
{
    playing = true;
    cursor = -(double)count_in * LD_TICKS_PER_BAR;
    played = cursor;
    next_beat = (int32_t)cursor;
    for (int h = 0; h < hit_count; ++h) {
        hits[h].skip = 0;
    }
    reposition();
    next_hit = 0; /* the hits on tick 0 are still to play */
}

void ld_engine_stop(void)
{
    playing = false;
    end_take();
    silence();
    xQueueReset(heard_queue);
}

bool ld_engine_playing(void)
{
    return playing;
}

void ld_engine_render(int16_t *pcm, size_t frames, uint16_t bpm, uint32_t first_frame)
{
    rendering = true;
    render_bpm = bpm;
    run_pending_call();
    uint8_t pad;
    while (xQueueReceive(hit_queue, &pad, 0) == pdTRUE) {
        trigger(pad);
        record_hit(pad);
    }
    double per_frame = ticks_per_frame(bpm);
    size_t done = 0;
    while (done < frames) {
        size_t segment = frames - done;
        if (segment > MIX_CHUNK) {
            segment = MIX_CHUNK;
        }
        if (playing) {
            play_due(first_frame + (uint32_t)done);
            size_t until = frames_to_next(per_frame);
            if (until < segment) {
                segment = until;
            }
        }
        mix(pcm + done, segment);
        if (playing) {
            cursor += segment * per_frame;
            played += segment * per_frame;
        }
        done += segment;
    }
    /* What falls due at the block's end starts the next block, so between
       blocks everything at or before the cursor has played. reposition()
       and record_hit() rely on that. */
    if (playing) {
        play_due(first_frame + (uint32_t)frames);
    }
}

/* ---- From the UI and touch tasks ------------------------------------------- */

static void make_click(int16_t *pcm, float frequency)
{
    for (int i = 0; i < CLICK_FRAMES; ++i) {
        float t = (float)i / LD_RATE;
        float envelope = expf(-t * 160.0f);
        pcm[i] = (int16_t)lrintf(32000.0f * envelope * sinf(2.0f * (float)M_PI * frequency * t));
    }
}

esp_err_t ld_engine_init(void)
{
    hit_queue = xQueueCreate(HIT_QUEUE_LENGTH, sizeof(uint8_t));
    heard_queue = xQueueCreate(HEARD_QUEUE_LENGTH, sizeof(ld_heard_t));
    call_mutex = xSemaphoreCreateMutex();
    call_done = xSemaphoreCreateBinary();
    hits = heap_caps_calloc(LD_MAX_HITS + 1, sizeof(ld_loop_hit_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    click_high = heap_caps_malloc(CLICK_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    click_low = heap_caps_malloc(CLICK_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hit_queue == NULL || heard_queue == NULL || call_mutex == NULL || call_done == NULL ||
        hits == NULL || click_high == NULL || click_low == NULL) {
        return ESP_ERR_NO_MEM;
    }
    make_click(click_high, 1760.0f);
    make_click(click_low, 1175.0f);
    for (int pad = 0; pad < LD_PADS; ++pad) {
        settings[pad].level = LD_LEVEL_DEFAULT;
    }
    return ESP_OK;
}

void ld_hit(int pad)
{
    if (pad < 0 || pad >= LD_PADS) {
        return;
    }
    uint8_t value = (uint8_t)pad;
    xQueueSend(hit_queue, &value, 0);
}

ld_pad_settings_t *ld_pad(int pad)
{
    return &settings[pad < 0 ? 0 : pad >= LD_PADS ? LD_PADS - 1 : pad];
}

const ld_sample_t *ld_sample(int pad)
{
    return &samples[pad < 0 ? 0 : pad >= LD_PADS ? LD_PADS - 1 : pad];
}

typedef struct {
    int pad;
    ld_sample_t sample;
} set_sample_t;

static void cut_voice(voice_t *voice, const int16_t *pcm)
{
    if (voice->pcm == pcm) {
        voice->pcm = NULL;
    }
}

static void do_set_sample(void *argument)
{
    const set_sample_t *set = argument;
    const int16_t *old = samples[set->pad].pcm;
    samples[set->pad] = set->sample;
    if (old == NULL) {
        return;
    }
    for (int pad = 0; pad < LD_PADS; ++pad) {
        if (samples[pad].pcm == old) {
            return; /* another pad still plays it (a kit's sound) */
        }
    }
    for (int pad = 0; pad < LD_PADS; ++pad) {
        cut_voice(&voices[pad].main, old);
        cut_voice(&voices[pad].tail, old);
    }
}

void ld_set_sample(int pad, const ld_sample_t *sample)
{
    if (pad < 0 || pad >= LD_PADS) {
        return;
    }
    set_sample_t set = {.pad = pad};
    if (sample != NULL && sample->pcm != NULL && sample->frames > 0) {
        set.sample = *sample;
    }
    call(do_set_sample, &set);
}

static void do_record(void *argument)
{
    if (*(bool *)argument) {
        begin_take();
    } else {
        end_take();
    }
}

void ld_record(bool on)
{
    call(do_record, &on);
}

bool ld_is_recording(void)
{
    return recording;
}

static void do_undo(void *argument)
{
    end_take();
    uint8_t take = highest_take();
    int kept = 0;
    for (int h = 0; h < hit_count; ++h) {
        if (hits[h].take != take) {
            hits[kept++] = hits[h];
        }
    }
    *(bool *)argument = kept != hit_count;
    hit_count = kept;
    reposition();
}

bool ld_undo(void)
{
    bool undone = false;
    call(do_undo, &undone);
    return undone;
}

static void do_clear(void *argument)
{
    (void)argument;
    hit_count = 0;
    reposition();
}

void ld_clear(void)
{
    call(do_clear, NULL);
}

/* After the loop's length changed: keeps the cursor in it and the beats
   on their ticks. */
static void fit_cursor(void)
{
    int total = loop_ticks();
    if (cursor >= total) {
        cursor = fmod(cursor, total);
    }
    if (cursor >= 0) {
        next_beat = ((int32_t)floor(cursor / LD_TICKS_PER_BEAT) + 1) * LD_TICKS_PER_BEAT;
    }
    reposition();
}

static void do_cycle_bars(void *argument)
{
    (void)argument;
    if (bars >= LD_MAX_BARS) {
        int kept = 0;
        for (int h = 0; h < hit_count; ++h) {
            if (hits[h].tick < LD_TICKS_PER_BAR) {
                hits[kept++] = hits[h];
            }
        }
        hit_count = kept;
        bars = 1;
    } else {
        int old_total = loop_ticks();
        int count = hit_count;
        for (int h = 0; h < count && hit_count < LD_MAX_HITS; ++h) {
            hits[hit_count] = hits[h];
            hits[hit_count].tick = (uint16_t)(hits[h].tick + old_total);
            hits[hit_count].skip = 0;
            ++hit_count;
        }
        bars *= 2;
    }
    fit_cursor();
}

void ld_cycle_bars(void)
{
    call(do_cycle_bars, NULL);
}

int ld_bars(void)
{
    return bars;
}

int ld_hit_count(void)
{
    return hit_count;
}

int ld_take_count(void)
{
    return take_count;
}

typedef struct {
    ld_loop_hit_t *hits;
    int count;
    int bars;
} loop_copy_t;

static void do_get_loop(void *argument)
{
    loop_copy_t *copy = argument;
    int n = hit_count < copy->count ? hit_count : copy->count;
    memcpy(copy->hits, hits, (size_t)n * sizeof(hits[0]));
    copy->count = n;
    copy->bars = bars;
}

int ld_get_loop(ld_loop_hit_t *out, int max, int *loop_bars)
{
    loop_copy_t copy = {.hits = out, .count = max};
    call(do_get_loop, &copy);
    if (loop_bars != NULL) {
        *loop_bars = copy.bars;
    }
    return copy.count;
}

static int compare_hits(const void *a, const void *b)
{
    const ld_loop_hit_t *x = a;
    const ld_loop_hit_t *y = b;
    return (int)x->tick - (int)y->tick;
}

static void do_set_loop(void *argument)
{
    loop_copy_t *copy = argument;
    end_take();
    int new_bars = copy->bars;
    if (new_bars != 1 && new_bars != 2 && new_bars != 4 && new_bars != 8) {
        new_bars = 1;
    }
    bars = new_bars;
    int total = loop_ticks();
    int n = 0;
    for (int h = 0; h < copy->count && n < LD_MAX_HITS; ++h) {
        if (copy->hits[h].pad < LD_PADS && copy->hits[h].tick < total) {
            hits[n] = copy->hits[h];
            hits[n].skip = 0;
            ++n;
        }
    }
    qsort(hits, (size_t)n, sizeof(hits[0]), compare_hits);
    hit_count = n;
    fit_cursor();
}

void ld_set_loop(const ld_loop_hit_t *loop, int count, int loop_bars)
{
    loop_copy_t copy = {.hits = (ld_loop_hit_t *)loop, .count = count, .bars = loop_bars};
    call(do_set_loop, &copy);
}

bool ld_next_heard(ld_heard_t *heard)
{
    ld_heard_t next;
    if (xQueuePeek(heard_queue, &next, 0) != pdTRUE) {
        return false;
    }
    if ((int32_t)(music_out_heard_frame() - next.frame) < 0) {
        return false;
    }
    return xQueueReceive(heard_queue, heard, 0) == pdTRUE;
}
