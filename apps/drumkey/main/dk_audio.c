#include "dk_audio.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "music_out.h"
#include "synth.h"

#define HIT_QUEUE_LENGTH 32
#define COMMAND_QUEUE_LENGTH 64
/* Enough for 2 s of latency at 200 BPM (27 steps). */
#define STEP_QUEUE_LENGTH 64
/* Keyboard loop notes on their way to the speaker: 2 s of a busy loop. */
#define HEARD_QUEUE_LENGTH 128
/* How often the render time is logged. */
#define LOAD_REPORT_US 10000000
#define BLOCK_US (1000000LL * MUSIC_OUT_BLOCK_FRAMES / MUSIC_OUT_RATE)

static const char *TAG = "drumkey";

typedef struct {
    uint8_t track;
    bool record;
} hit_t;

typedef enum {
    COMMAND_NOTE_ON,
    COMMAND_NOTE_OFF,
    COMMAND_SUSTAIN,
    COMMAND_ALL_OFF,
    COMMAND_RECORD,
    COMMAND_UNDO,
    COMMAND_CLEAR,
    COMMAND_BARS,
} command_type_t;

typedef struct {
    uint8_t type;
    uint8_t note;
    uint8_t instrument;
    uint8_t value;
} command_t;

typedef struct {
    uint32_t frame;
    uint8_t note;
    bool on;
} heard_note_t;

volatile looper_settings_t keys_settings = {
    .bpm = 120,
    .metronome = false,
    .count_in = false,
    .quantize = QUANTIZE_OFF,
};

static drum_pattern_t *pattern;
static drum_engine_t engine;
static QueueHandle_t hits;
static QueueHandle_t steps;
static QueueHandle_t commands;
static QueueHandle_t heard;
static volatile bool want_playing;
/* The drums render here, the keys into the output, then they are mixed. */
static int16_t drum_pcm[MUSIC_OUT_BLOCK_FRAMES];

/* ---- In the audio task --------------------------------------------------- */

static void on_step(uint64_t frame, uint16_t step, void *context)
{
    (void)context;
    audio_step_t event = {.frame = (uint32_t)frame, .step = step};
    xQueueSend(steps, &event, 0);
}

static void on_loop_note(uint32_t frame, int note, bool on)
{
    heard_note_t event = {.frame = frame, .note = (uint8_t)note, .on = on};
    xQueueSend(heard, &event, 0);
}

static void take_hits(void)
{
    hit_t hit;
    while (xQueueReceive(hits, &hit, 0) == pdTRUE) {
        engine_hit(&engine, hit.track);
        if (hit.record) {
            /* The player hears the loop the speaker's latency late, so the
               hit belongs at the step they were hearing. */
            int step = engine_quantize(&engine, music_out_latency_frames());
            if (step >= 0) {
                pattern->steps[step / DRUM_STEPS][hit.track] |= (uint16_t)(1u << (step % DRUM_STEPS));
            }
        }
    }
}

static void run_commands(void)
{
    command_t command;
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
        switch (command.type) {
        case COMMAND_NOTE_ON:
            looper_live_on(command.note, command.instrument, music_out_latency_frames());
            break;
        case COMMAND_NOTE_OFF:
            looper_live_off(command.note, command.instrument);
            break;
        case COMMAND_SUSTAIN:
            looper_sustain(command.value != 0);
            break;
        case COMMAND_ALL_OFF:
            looper_live_all_off();
            break;
        case COMMAND_RECORD:
            looper_record(command.value != 0);
            break;
        case COMMAND_UNDO:
            looper_undo();
            break;
        case COMMAND_CLEAR:
            looper_clear();
            break;
        case COMMAND_BARS:
            looper_cycle_bars();
            break;
        default:
            break;
        }
    }
}

/* Starts or stops both loops in the same block, so they stay in step.
   True when the speaker's buffer should go. */
static bool run_transport(void)
{
    if (want_playing && !engine.playing) {
        engine_start(&engine);
        looper_play();
    } else if (!want_playing && engine.playing) {
        engine_stop(&engine);
        engine_silence(&engine);
        looper_stop();
        synth_silence();
        xQueueReset(steps);
        xQueueReset(heard);
        return true;
    }
    return false;
}

/* The engines' soft clip, on the sum: linear when quiet, rounding off
   toward full scale. */
static int16_t mix(int16_t keys, int16_t drums)
{
    float x = ((float)keys + (float)drums) / 32768.0f;
    if (x > 3.0f) {
        x = 1.0f;
    } else if (x < -3.0f) {
        x = -1.0f;
    } else {
        x = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }
    return (int16_t)lrintf(x * 32767.0f);
}

/* Logs how long a block takes to render, against the 8 ms it lasts. */
static void report_load(int64_t start, int64_t took)
{
    static int64_t report_at;
    static int64_t total_us;
    static int64_t worst_us;
    static uint32_t blocks;
    total_us += took;
    worst_us = took > worst_us ? took : worst_us;
    ++blocks;
    if (start >= report_at) {
        if (blocks > 1) {
            ESP_LOGI(TAG, "Render: average %lld us, worst %lld us of %lld us per block",
                     total_us / blocks, worst_us, BLOCK_US);
        }
        report_at = start + LOAD_REPORT_US;
        total_us = worst_us = 0;
        blocks = 0;
    }
}

/* music_out's render callback. */
static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    (void)speaker_attached;
    (void)context;
    int64_t start = esp_timer_get_time();
    bool flush = run_transport();
    run_commands();
    take_hits();
    looper_settings_t settings = keys_settings;
    settings.bpm = pattern->bpm;
    settings.count_in = false;
    uint32_t first_frame = music_out_frames_written();
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > MUSIC_OUT_BLOCK_FRAMES) {
            chunk = MUSIC_OUT_BLOCK_FRAMES;
        }
        looper_render(pcm + done, chunk, first_frame + (uint32_t)done, &settings);
        engine_render(&engine, drum_pcm, chunk);
        for (size_t i = 0; i < chunk; ++i) {
            pcm[done + i] = mix(pcm[done + i], drum_pcm[i]);
        }
        done += chunk;
    }
    report_load(start, esp_timer_get_time() - start);
    return flush;
}

/* The latency may have changed: the playhead and lit keys would be off. */
static void on_output_change(void *context)
{
    (void)context;
    xQueueReset(steps);
    xQueueReset(heard);
}

esp_err_t dk_audio_start(drum_pattern_t *loop_pattern, QueueHandle_t touch_queue)
{
    pattern = loop_pattern;
    hits = xQueueCreate(HIT_QUEUE_LENGTH, sizeof(hit_t));
    steps = xQueueCreate(STEP_QUEUE_LENGTH, sizeof(audio_step_t));
    commands = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_t));
    heard = xQueueCreate(HEARD_QUEUE_LENGTH, sizeof(heard_note_t));
    if (hits == NULL || steps == NULL || commands == NULL || heard == NULL) {
        return ESP_ERR_NO_MEM;
    }
    engine_init(&engine, pattern, on_step, NULL);
    synth_init();
    looper_init(on_loop_note);
    music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
    config.title = " DRUM&KEY";
    config.render = render;
    config.on_change = on_output_change;
    config.task_stack = 8192; /* the synth mixes in float on the stack */
    config.task_core = 1;     /* Wi-Fi runs on core 0, and the UI mostly does */
    return music_out_start(&config, touch_queue);
}

/* ---- From the UI and touch tasks ----------------------------------------- */

drum_pattern_t *dk_pattern(void)
{
    return pattern;
}

void dk_play(bool play)
{
    want_playing = play;
}

bool dk_is_playing(void)
{
    return want_playing;
}

uint16_t dk_tempo(void)
{
    return pattern->bpm;
}

void dk_set_tempo(int bpm)
{
    int low = DRUM_BPM_MIN > MUSIC_TEMPO_MIN ? DRUM_BPM_MIN : MUSIC_TEMPO_MIN;
    int high = DRUM_BPM_MAX < MUSIC_TEMPO_MAX ? DRUM_BPM_MAX : MUSIC_TEMPO_MAX;
    pattern->bpm = (uint16_t)(bpm < low ? low : bpm > high ? high : bpm);
}

void drums_hit(int track, bool record)
{
    hit_t hit = {.track = (uint8_t)track, .record = record};
    xQueueSend(hits, &hit, 0);
}

bool drums_next_heard_step(audio_step_t *step)
{
    audio_step_t next;
    if (xQueuePeek(steps, &next, 0) != pdTRUE) {
        return false;
    }
    if ((int32_t)(music_out_heard_frame() - next.frame) < 0) {
        return false;
    }
    return xQueueReceive(steps, step, 0) == pdTRUE;
}

static void send(command_type_t type, int note, int instrument, int value)
{
    command_t command = {
        .type = (uint8_t)type,
        .note = (uint8_t)note,
        .instrument = (uint8_t)instrument,
        .value = (uint8_t)value,
    };
    xQueueSend(commands, &command, 0);
}

void keys_note_on(int note, int instrument)
{
    send(COMMAND_NOTE_ON, note, instrument, 0);
}

void keys_note_off(int note, int instrument)
{
    send(COMMAND_NOTE_OFF, note, instrument, 0);
}

void keys_sustain(bool down)
{
    send(COMMAND_SUSTAIN, 0, 0, down);
}

void keys_all_notes_off(void)
{
    send(COMMAND_ALL_OFF, 0, 0, 0);
}

void keys_record(bool on)
{
    send(COMMAND_RECORD, 0, 0, on);
}

void keys_undo(void)
{
    send(COMMAND_UNDO, 0, 0, 0);
}

void keys_clear(void)
{
    send(COMMAND_CLEAR, 0, 0, 0);
}

void keys_cycle_bars(void)
{
    send(COMMAND_BARS, 0, 0, 0);
}

bool keys_next_heard_note(int *note, bool *on)
{
    heard_note_t next;
    if (xQueuePeek(heard, &next, 0) != pdTRUE) {
        return false;
    }
    if ((int32_t)(music_out_heard_frame() - next.frame) < 0) {
        return false;
    }
    if (xQueueReceive(heard, &next, 0) != pdTRUE) {
        return false;
    }
    *note = next.note;
    *on = next.on;
    return true;
}
