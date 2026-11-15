#include "audio.h"

#include "engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "music_out.h"

#define COMMAND_QUEUE_LENGTH 64
#define BLOCK_US (1000000LL * MUSIC_OUT_BLOCK_FRAMES / MUSIC_OUT_RATE)
#define LOAD_WINDOW_US 1000000
#define LOG_EVERY_US 10000000

static const char *TAG = "odin_s3";

typedef enum {
    COMMAND_NOTE_ON,
    COMMAND_NOTE_OFF,
    COMMAND_ALL_OFF,
    COMMAND_PATCH,
    COMMAND_MODWHEEL,
    COMMAND_PITCHBEND,
    COMMAND_XY,
    COMMAND_BPM,
} command_type_t;

typedef struct {
    uint8_t type;
    uint8_t note;
    uint8_t velocity;
    float a, b;
    const odin_patch_t *patch;
} command_t;

static QueueHandle_t commands;
static volatile int load_average;
static volatile int load_worst;
static volatile int voices;

static void send(const command_t *command)
{
    xQueueSend(commands, command, 0);
}

static void run_commands(void)
{
    command_t c;
    while (xQueueReceive(commands, &c, 0) == pdTRUE) {
        switch (c.type) {
        case COMMAND_NOTE_ON:
            engine_note_on(c.note, c.velocity);
            break;
        case COMMAND_NOTE_OFF:
            engine_note_off(c.note);
            break;
        case COMMAND_ALL_OFF:
            engine_all_notes_off();
            break;
        case COMMAND_PATCH:
            engine_load_patch(c.patch);
            break;
        case COMMAND_MODWHEEL:
            engine_set_modwheel(c.a);
            break;
        case COMMAND_PITCHBEND:
            engine_set_pitchbend(c.a);
            break;
        case COMMAND_XY:
            engine_set_xy(c.a, c.b);
            break;
        case COMMAND_BPM:
            engine_set_bpm(c.a);
            break;
        default:
            break;
        }
    }
}

/* music_out's render callback, in the audio task. */
static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    (void)speaker_attached;
    (void)context;
    static int64_t window_start, log_at;
    static int64_t total_us, worst_us;
    static uint32_t blocks;
    int64_t start = esp_timer_get_time();
    run_commands();
    engine_render(pcm, frames);
    int64_t took = esp_timer_get_time() - start;
    voices = engine_active_voices();
    total_us += took;
    worst_us = took > worst_us ? took : worst_us;
    ++blocks;
    if (start - window_start >= LOAD_WINDOW_US) {
        load_average = (int)(100 * total_us / blocks / BLOCK_US);
        load_worst = (int)(100 * worst_us / BLOCK_US);
        if (start >= log_at && worst_us > BLOCK_US / 4) {
            ESP_LOGI(TAG, "Render: average %lld us, worst %lld us of %lld us per block, %d voices",
                     total_us / blocks, worst_us, BLOCK_US, voices);
            log_at = start + LOG_EVERY_US;
        }
        window_start = start;
        total_us = worst_us = 0;
        blocks = 0;
    }
    return false;
}

esp_err_t audio_start(QueueHandle_t touch_queue)
{
    commands = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_t));
    if (commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!engine_init()) {
        ESP_LOGW(TAG, "No PSRAM for the delay line: the delay is off");
    }
    music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
    config.title = " ODIN S3";
    config.render = render;
    config.task_stack = 8192;
    config.task_core = 1; /* Wi-Fi runs on core 0, and the UI mostly does */
    return music_out_start(&config, touch_queue);
}

void audio_note_on(int note, int velocity)
{
    send(&(command_t){.type = COMMAND_NOTE_ON, .note = (uint8_t)note, .velocity = (uint8_t)velocity});
}

void audio_note_off(int note)
{
    send(&(command_t){.type = COMMAND_NOTE_OFF, .note = (uint8_t)note});
}

void audio_all_notes_off(void)
{
    send(&(command_t){.type = COMMAND_ALL_OFF});
}

void audio_load_patch(const odin_patch_t *patch)
{
    send(&(command_t){.type = COMMAND_PATCH, .patch = patch});
}

void audio_set_modwheel(float value)
{
    send(&(command_t){.type = COMMAND_MODWHEEL, .a = value});
}

void audio_set_pitchbend(float value)
{
    send(&(command_t){.type = COMMAND_PITCHBEND, .a = value});
}

void audio_set_xy(float x, float y)
{
    send(&(command_t){.type = COMMAND_XY, .a = x, .b = y});
}

void audio_set_bpm(int bpm)
{
    send(&(command_t){.type = COMMAND_BPM, .a = (float)bpm});
}

void audio_load(int *average, int *worst)
{
    *average = load_average;
    *worst = load_worst;
}

int audio_voices(void)
{
    return voices;
}
