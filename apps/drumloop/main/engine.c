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
    memset(engine->roll_left, 0, sizeof(engine->roll_left));
}

void engine_stop(drum_engine_t *engine)
{
    engine->playing = false;
    memset(engine->roll_left, 0, sizeof(engine->roll_left));
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
        voice->fade_frac = voice->frac;
        voice->fade_rate = voice->rate;
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
    voice->frac = 0.0f;
    /* PITCH retunes the hit, as a sampler does: faster and higher, or
       slower and lower. Set at the hit, so a sweep changes the next one. */
    int pitch = engine->pattern->tone[track].pitch;
    voice->rate = pitch == 0 ? 1.0f : powf(2.0f, (float)pitch / 12.0f);
    /* DECAY plays part of the hit, fading over the last half of that, so
       an open hat or a long boom becomes a short one. */
    uint32_t frames = voice->sound != NULL ? voice->sound->frames : 0;
    int decay = engine->pattern->tone[track].decay;
    if (decay > 95) {
        decay = 95;
    }
    voice->end = decay == 0 ? frames : (uint32_t)((uint64_t)frames * (uint32_t)(100 - decay) / 100u);
    voice->fade_from = decay == 0 ? frames : voice->end / 2;
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
    double per_step = engine_frames_per_step(pattern->bpm);
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        engine->roll_left[track] = 0;
        bool muted = (pattern->muted >> track) & 1;
        if (!muted && ((pattern->steps[bar][track] >> step) & 1)) {
            engine_hit(engine, track);
            int rolls = drum_rolls(pattern, bar, track, step);
            if (rolls > 1) {
                engine->roll_left[track] = (uint8_t)(rolls - 1);
                engine->roll_gap[track] = per_step / rolls;
                engine->roll_frame[track] = engine->next_step_frame + per_step / rolls;
            }
        }
    }
    if (engine->on_step != NULL) {
        engine->on_step((uint64_t)llround(engine->next_step_frame), engine->next_step,
                        engine->context);
    }
    engine->next_step = (uint16_t)((engine->next_step + 1) % total);
    engine->next_step_frame += per_step;
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

/* A sound's sample at pos plus frac, between it and the next. */
static inline float sound_at(const drum_sound_t *sound, uint32_t pos, float frac)
{
    float a = sound->pcm[pos];
    float b = pos + 1 < sound->frames ? sound->pcm[pos + 1] : 0.0f;
    return a + (b - a) * frac;
}

/* Steps a pitched read position on by rate. */
static inline void advance(uint32_t *pos, float *frac, float rate)
{
    *frac += rate;
    uint32_t whole = (uint32_t)*frac;
    *pos += whole;
    *frac -= (float)whole;
}

/* Adds a track's hit, and the one fading out under it, into acc. */
static void add_voice(drum_voice_t *voice, float *acc, size_t frames)
{
    const drum_sound_t *sound = voice->sound;
    if (sound != NULL) {
        float gain = sound->gain / 32768.0f;
        size_t i = 0;
        uint32_t end = voice->end == 0 || voice->end > sound->frames ? sound->frames : voice->end;
        float rate = voice->rate == 0.0f ? 1.0f : voice->rate;
        if (rate == 1.0f && end == sound->frames) {
            for (; i < frames && voice->pos < end; ++i) {
                acc[i] += sound->pcm[voice->pos++] * gain;
            }
        } else {
            float fade_span = (float)(end - (voice->fade_from < end ? voice->fade_from : end));
            for (; i < frames && voice->pos < end; ++i) {
                float g = gain;
                if (voice->pos >= voice->fade_from && fade_span > 0.0f) {
                    g *= (float)(end - voice->pos) / fade_span;
                }
                acc[i] += sound_at(sound, voice->pos, voice->frac) * g;
                advance(&voice->pos, &voice->frac, rate);
            }
        }
        if (voice->pos >= end) {
            voice->sound = NULL;
        }
    }
    const drum_sound_t *fading = voice->fade_sound;
    float fade_rate = voice->fade_rate == 0.0f ? 1.0f : voice->fade_rate;
    for (size_t i = 0; i < frames && voice->fade_left > 0; ++i) {
        if (voice->fade_pos >= fading->frames) {
            voice->fade_left = 0;
            break;
        }
        float fade = (float)voice->fade_left / RETRIGGER_FADE;
        acc[i] += sound_at(fading, voice->fade_pos, voice->fade_frac) * (fading->gain / 32768.0f) * fade;
        advance(&voice->fade_pos, &voice->fade_frac, fade_rate);
        --voice->fade_left;
    }
}

static void mix(drum_engine_t *engine, int16_t *out, size_t frames)
{
    float acc[MIX_CHUNK];
    float track_acc[MIX_CHUNK];
    memset(acc, 0, frames * sizeof(float));
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        drum_voice_t *voice = &engine->voices[track];
        const tone_settings_t *tone = &engine->pattern->tone[track];
        tone_state_t *state = &engine->tones[track];
        if (tone_sound_flat(tone)) {
            add_voice(voice, acc, frames); /* nothing to run it through */
            continue;
        }
        /* A track with a tone is mixed on its own and run through it. A
           filter can ring on after the hit, so it runs until it is quiet. */
        tone_update(state, tone);
        bool sounding = voice->sound != NULL || voice->fade_left > 0;
        if (!sounding && !tone_ringing(state)) {
            continue;
        }
        memset(track_acc, 0, frames * sizeof(float));
        add_voice(voice, track_acc, frames);
        tone_process(state, track_acc, frames);
        for (size_t i = 0; i < frames; ++i) {
            acc[i] += track_acc[i];
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
            /* A roll's hits are always due before the next step. */
            while (true) {
                bool hit = false;
                for (int track = 0; track < DRUM_TRACKS; ++track) {
                    if (engine->roll_left[track] > 0 && engine->roll_frame[track] <= (double)engine->frame) {
                        engine_hit(engine, track);
                        engine->roll_left[track]--;
                        engine->roll_frame[track] += engine->roll_gap[track];
                        hit = true;
                    }
                }
                if (hit) {
                    continue;
                }
                if (engine->next_step_frame <= (double)engine->frame) {
                    start_step(engine);
                } else {
                    break;
                }
            }
            /* Mix only up to the next hit, so it starts on its frame. */
            double next = engine->next_step_frame;
            for (int track = 0; track < DRUM_TRACKS; ++track) {
                if (engine->roll_left[track] > 0 && engine->roll_frame[track] < next) {
                    next = engine->roll_frame[track];
                }
            }
            double until = ceil(next - (double)engine->frame);
            if (until < (double)segment) {
                segment = (size_t)until;
            }
        }
        mix(engine, out + done, segment);
        engine->frame += segment;
        done += segment;
    }
}
