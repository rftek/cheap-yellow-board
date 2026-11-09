#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "music_out.h"
#include "synth.h"

#define COMMAND_QUEUE_LENGTH 64
/* Loop notes on their way to the speaker: 2 s of a busy loop. */
#define HEARD_QUEUE_LENGTH 128
#define DEFAULT_BPM 120

typedef enum {
    COMMAND_NOTE_ON,
    COMMAND_NOTE_OFF,
    COMMAND_SUSTAIN,
    COMMAND_ALL_OFF,
    COMMAND_PLAY,
    COMMAND_STOP,
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

volatile looper_settings_t audio_settings = {
    .bpm = DEFAULT_BPM,
    .metronome = false,
    .count_in = true,
    .quantize = QUANTIZE_OFF,
};

static QueueHandle_t commands;
static QueueHandle_t heard;

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

static void on_loop_note(uint32_t frame, int note, bool on)
{
    heard_note_t event = {.frame = frame, .note = (uint8_t)note, .on = on};
    xQueueSend(heard, &event, 0);
}

/* Runs the queued commands; true when the speaker's buffer should go. */
static bool run_commands(void)
{
    bool flush = false;
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
        case COMMAND_PLAY:
            looper_play();
            break;
        case COMMAND_STOP:
            looper_stop();
            synth_silence();
            xQueueReset(heard);
            flush = true;
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
    return flush;
}

/* music_out's render callback, in the audio task. */
static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    (void)speaker_attached;
    (void)context;
    bool flush = run_commands();
    looper_settings_t settings = audio_settings;
    looper_render(pcm, frames, music_out_frames_written(), &settings);
    return flush;
}

/* The latency may have changed: lit keys would come late or early. */
static void on_output_change(void *context)
{
    (void)context;
    xQueueReset(heard);
}

esp_err_t audio_start(QueueHandle_t touch_queue)
{
    commands = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_t));
    heard = xQueueCreate(HEARD_QUEUE_LENGTH, sizeof(heard_note_t));
    if (commands == NULL || heard == NULL) {
        return ESP_ERR_NO_MEM;
    }
    synth_init();
    looper_init(on_loop_note);
    music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
    config.title = " KEYBOARD";
    config.render = render;
    config.on_change = on_output_change;
    config.task_stack = 8192; /* the synth mixes in float on the stack */
    return music_out_start(&config, touch_queue);
}

void audio_note_on(int note, int instrument)
{
    send(COMMAND_NOTE_ON, note, instrument, 0);
}

void audio_note_off(int note, int instrument)
{
    send(COMMAND_NOTE_OFF, note, instrument, 0);
}

void audio_sustain(bool down)
{
    send(COMMAND_SUSTAIN, 0, 0, down);
}

void audio_all_notes_off(void)
{
    send(COMMAND_ALL_OFF, 0, 0, 0);
}

void audio_play(void)
{
    send(COMMAND_PLAY, 0, 0, 0);
}

void audio_stop(void)
{
    send(COMMAND_STOP, 0, 0, 0);
}

void audio_record(bool on)
{
    send(COMMAND_RECORD, 0, 0, on);
}

void audio_undo(void)
{
    send(COMMAND_UNDO, 0, 0, 0);
}

void audio_clear(void)
{
    send(COMMAND_CLEAR, 0, 0, 0);
}

void audio_cycle_bars(void)
{
    send(COMMAND_BARS, 0, 0, 0);
}

bool audio_next_heard_note(int *note, bool *on)
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
