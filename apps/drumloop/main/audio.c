#include "audio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BLOCK_FRAMES AIRPLAY_FRAMES_PER_PACKET
#define HIT_QUEUE_LENGTH 32
/* Enough for 2 s of latency at 200 BPM (27 steps). */
#define STEP_QUEUE_LENGTH 64
#define WRITE_TIMEOUT_MS 500

typedef struct {
    uint8_t track;
    bool record;
} hit_t;

static const char *TAG = "audio";

static drum_pattern_t *pattern;
static drum_engine_t engine;
/* Covers speaker and every call on it, so the UI's volume changes and
   detaching never run during a write. */
static SemaphoreHandle_t lock;
static airplay_sender_handle_t speaker;
static QueueHandle_t hits;
static QueueHandle_t steps;
static volatile uint32_t frames_written;
static volatile uint32_t latency_frames;
static volatile bool want_playing;
/* Silent mode: no speaker, the engine runs on the clock alone. */
static bool silent;
static bool silent_restart;

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
            /* The player hears the loop latency_frames late, so the hit
               belongs at the step they were hearing. */
            int step = engine_quantize(&engine, latency_frames);
            if (step >= 0) {
                pattern->steps[step / DRUM_STEPS][hit.track] |= (uint16_t)(1u << (step % DRUM_STEPS));
            }
        }
    }
}

/* Applies transport changes and hits, then renders one block. target is
   the speaker, or NULL in silent mode. */
static void render_block(int16_t *pcm, airplay_sender_handle_t target)
{
    if (want_playing && !engine.playing) {
        engine_start(&engine);
    } else if (!want_playing && engine.playing) {
        engine_stop(&engine);
        engine_silence(&engine);
        if (target != NULL) {
            airplay_sender_stop(target);
        }
        xQueueReset(steps);
    }
    take_hits();
    engine_render(&engine, pcm, BLOCK_FRAMES);
}

static void audio_task(void *argument)
{
    (void)argument;
    static int16_t pcm[BLOCK_FRAMES];
    int64_t silent_start_us = 0;
    uint64_t silent_frames = 0;
    while (true) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (speaker != NULL) {
            render_block(pcm, speaker);
            esp_err_t result = airplay_sender_write(speaker, pcm, BLOCK_FRAMES,
                                                    pdMS_TO_TICKS(WRITE_TIMEOUT_MS), NULL);
            xSemaphoreGive(lock);
            frames_written += BLOCK_FRAMES;
            if (result != ESP_OK) {
                /* The speaker went away; the UI hears of it from the
                   sender's event and detaches. Don't spin meanwhile. */
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
        bool run_silent = silent;
        if (silent_restart) {
            silent_restart = false;
            silent_start_us = esp_timer_get_time();
            silent_frames = 0;
        }
        xSemaphoreGive(lock);
        if (run_silent) {
            /* No speaker to pace the writes, so the clock does: render
               every block that is due, as if a speaker played it. */
            uint64_t due = (uint64_t)(esp_timer_get_time() - silent_start_us) * DRUM_RATE / 1000000;
            while (silent_frames + BLOCK_FRAMES <= due) {
                render_block(pcm, NULL);
                silent_frames += BLOCK_FRAMES;
                frames_written += BLOCK_FRAMES;
            }
            vTaskDelay(1);
            continue;
        }
        if (engine.playing) {
            engine_stop(&engine);
        }
        engine_silence(&engine);
        xQueueReset(hits);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t audio_start(drum_pattern_t *loop_pattern)
{
    pattern = loop_pattern;
    lock = xSemaphoreCreateMutex();
    hits = xQueueCreate(HIT_QUEUE_LENGTH, sizeof(hit_t));
    steps = xQueueCreate(STEP_QUEUE_LENGTH, sizeof(audio_step_t));
    if (lock == NULL || hits == NULL || steps == NULL) {
        return ESP_ERR_NO_MEM;
    }
    engine_init(&engine, pattern, on_step, NULL);
    /* Above the UI, so drawing never delays a packet. */
    BaseType_t created = xTaskCreate(audio_task, "audio", 6144, NULL, 7, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void audio_set_speaker(airplay_sender_handle_t new_speaker)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    speaker = new_speaker;
    silent = false;
    want_playing = false;
    if (new_speaker != NULL) {
        latency_frames =
            (uint32_t)((uint64_t)airplay_sender_latency_ms(new_speaker) * DRUM_RATE / 1000);
        ESP_LOGI(TAG, "Speaker attached, latency %lu frames", (unsigned long)latency_frames);
    }
    xQueueReset(steps);
    xSemaphoreGive(lock);
}

void audio_set_silent(bool on)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    silent = on && speaker == NULL;
    silent_restart = silent;
    want_playing = false;
    latency_frames = 0;
    xQueueReset(steps);
    xSemaphoreGive(lock);
}

bool audio_has_speaker(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool attached = speaker != NULL;
    xSemaphoreGive(lock);
    return attached;
}

esp_err_t audio_set_volume(int volume)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t result = speaker != NULL ? airplay_sender_set_volume(speaker, volume)
                                       : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(lock);
    return result;
}

int audio_get_volume(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int volume = speaker != NULL ? airplay_sender_get_volume(speaker) : 0;
    xSemaphoreGive(lock);
    return volume;
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

uint32_t audio_heard_frame(void)
{
    return frames_written - latency_frames;
}

bool audio_next_heard_step(audio_step_t *step)
{
    audio_step_t next;
    if (xQueuePeek(steps, &next, 0) != pdTRUE) {
        return false;
    }
    if ((int32_t)(audio_heard_frame() - next.frame) < 0) {
        return false;
    }
    return xQueueReceive(steps, step, 0) == pdTRUE;
}
