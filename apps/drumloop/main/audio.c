#include "audio.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "music_out.h"

#define HIT_QUEUE_LENGTH 32
/* Enough for 2 s of latency at 200 BPM (27 steps). */
#define STEP_QUEUE_LENGTH 64

typedef struct {
    uint8_t track;
    bool record;
} hit_t;

static drum_pattern_t *pattern;
static drum_engine_t engine;
static QueueHandle_t hits;
static QueueHandle_t steps;
static volatile bool want_playing;

static void on_step(uint64_t frame, uint16_t step, void *context)
{
    (void)context;
    audio_step_t event = {.frame = (uint32_t)frame, .step = step};
    xQueueSend(steps, &event, 0);
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

/* music_out's render callback, in the audio task: applies transport
   changes and hits, then renders one block. */
static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    (void)speaker_attached;
    (void)context;
    bool flush = false;
    if (want_playing && !engine.playing) {
        engine_start(&engine);
    } else if (!want_playing && engine.playing) {
        engine_stop(&engine);
        engine_silence(&engine);
        xQueueReset(steps);
        flush = true;
    }
    take_hits();
    engine_render(&engine, pcm, frames);
    return flush;
}

/* The loop keeps playing across an output change; the playhead picks up
   the new latency from the next step. */
static void on_output_change(void *context)
{
    (void)context;
    xQueueReset(steps);
}

esp_err_t audio_start(drum_pattern_t *loop_pattern, QueueHandle_t touch_queue)
{
    pattern = loop_pattern;
    hits = xQueueCreate(HIT_QUEUE_LENGTH, sizeof(hit_t));
    steps = xQueueCreate(STEP_QUEUE_LENGTH, sizeof(audio_step_t));
    if (hits == NULL || steps == NULL) {
        return ESP_ERR_NO_MEM;
    }
    engine_init(&engine, pattern, on_step, NULL);
    music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
    config.title = " DRUMLOOP";
    config.render = render;
    config.on_change = on_output_change;
    return music_out_start(&config, touch_queue);
}

void audio_hit(int track, bool record)
{
    hit_t hit = {.track = (uint8_t)track, .record = record};
    xQueueSend(hits, &hit, 0);
}

void audio_play(bool play)
{
    want_playing = play;
}

bool audio_is_playing(void)
{
    return want_playing;
}

bool audio_next_heard_step(audio_step_t *step)
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
