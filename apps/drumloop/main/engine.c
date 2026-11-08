#include "engine.h"

#include <math.h>
#include <string.h>

/* A retriggered sound fades out over 5 ms instead of stopping dead. */
#define RETRIGGER_FADE (DRUM_RATE / 200)
#define MIX_CHUNK 256
#define MASTER_GAIN 0.8f

void engine_init(drum_engine_t *engine, const drum_pattern_t *pattern,
                 drum_step_cb_t on_step, void *context)
{
    memset(engine, 0, sizeof(*engine));
    engine->pattern = pattern;
    engine->on_step = on_step;
    engine->context = context;
    engine_silence(engine);
}

double engine_frames_per_step(uint16_t bpm)
{
    if (bpm < DRUM_BPM_MIN) {
        bpm = DRUM_BPM_MIN;
    }
    /* Four steps to a beat. */
    return DRUM_RATE * 60.0 / (bpm * 4.0);
}

void engine_start(drum_engine_t *engine)
{
    engine->playing = true;
    engine->next_step = 0;
    engine->next_step_frame = (double)engine->frame;
}

void engine_stop(drum_engine_t *engine)
{
    engine->playing = false;
}

void engine_silence(drum_engine_t *engine)
{
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        engine->voices[track].sound = NULL;
        engine->voices[track].fade_left = 0;
    }
}

/* Moves a sounding hit to the fade slot, so it dies away in 5 ms. */
static void fade_out(drum_voice_t *voice)
{
    if (voice->sound != NULL) {
        voice->fade_sound = voice->sound;
        voice->fade_pos = voice->pos;
        voice->fade_left = RETRIGGER_FADE;
    }
    voice->sound = NULL;
}

void engine_hit(drum_engine_t *engine, int track)
{
    if (track < 0 || track >= DRUM_TRACKS) {
        return;
    }
    /* As on the 808, the closed hat chokes the open one. */
    if (track == DRUM_CLOSED_HAT) {
        fade_out(&engine->voices[DRUM_OPEN_HAT]);
    }
    drum_voice_t *voice = &engine->voices[track];
    fade_out(voice);
    voice->sound = kit_sound(engine->pattern->kit, track);
    voice->pos = 0;
}

static uint16_t loop_steps(const drum_engine_t *engine)
{
    uint8_t bars = engine->pattern->bars;
    if (bars < 1 || bars > DRUM_MAX_BARS) {
        bars = 1;
    }
    return (uint16_t)(bars * DRUM_STEPS);
}

static void start_step(drum_engine_t *engine)
{
    const drum_pattern_t *pattern = engine->pattern;
    uint16_t total = loop_steps(engine);
    if (engine->next_step >= total) {
        /* The loop was shortened while it played. */
        engine->next_step %= total;
    }
    int bar = engine->next_step / DRUM_STEPS;
    int step = engine->next_step % DRUM_STEPS;
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        bool muted = (pattern->muted >> track) & 1;
        if (!muted && ((pattern->steps[bar][track] >> step) & 1)) {
            engine_hit(engine, track);
        }
    }
    if (engine->on_step != NULL) {
        engine->on_step((uint64_t)llround(engine->next_step_frame), engine->next_step,
                        engine->context);
    }
    engine->next_step = (uint16_t)((engine->next_step + 1) % total);
    engine->next_step_frame += engine_frames_per_step(pattern->bpm);
}

int engine_quantize(const drum_engine_t *engine, uint32_t frames_ago)
{
    if (!engine->playing) {
        return -1;
    }
    double per_step = engine_frames_per_step(engine->pattern->bpm);
    /* Position now, in steps: the next step minus the time until it. */
    double position = engine->next_step -
                      (engine->next_step_frame - (double)engine->frame) / per_step;
    position -= frames_ago / per_step;
    int total = loop_steps(engine);
    long step = lround(position) % total;
    if (step < 0) {
        step += total;
    }
    return (int)step;
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

static void mix(drum_engine_t *engine, int16_t *out, size_t frames)
{
    float acc[MIX_CHUNK];
    memset(acc, 0, frames * sizeof(float));
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        drum_voice_t *voice = &engine->voices[track];
        const drum_sound_t *sound = voice->sound;
        if (sound != NULL) {
            float gain = sound->gain / 32768.0f;
            size_t i = 0;
            for (; i < frames && voice->pos < sound->frames; ++i) {
                acc[i] += sound->pcm[voice->pos++] * gain;
            }
            if (voice->pos >= sound->frames) {
                voice->sound = NULL;
            }
        }
        const drum_sound_t *fading = voice->fade_sound;
        for (size_t i = 0; i < frames && voice->fade_left > 0; ++i) {
            if (voice->fade_pos >= fading->frames) {
                voice->fade_left = 0;
                break;
            }
            float fade = (float)voice->fade_left / RETRIGGER_FADE;
            acc[i] += fading->pcm[voice->fade_pos++] * (fading->gain / 32768.0f) * fade;
            --voice->fade_left;
        }
    }
    for (size_t i = 0; i < frames; ++i) {
        out[i] = (int16_t)lrintf(soft_clip(acc[i] * MASTER_GAIN) * 32767.0f);
    }
}

void engine_render(drum_engine_t *engine, int16_t *out, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        size_t segment = frames - done;
        if (segment > MIX_CHUNK) {
            segment = MIX_CHUNK;
        }
        if (engine->playing) {
            while (engine->next_step_frame <= (double)engine->frame) {
                start_step(engine);
            }
            /* Mix only up to the next step, so it starts on its frame. */
            double until = ceil(engine->next_step_frame - (double)engine->frame);
            if (until < (double)segment) {
                segment = (size_t)until;
            }
        }
        mix(engine, out + done, segment);
        engine->frame += segment;
        done += segment;
    }
}
