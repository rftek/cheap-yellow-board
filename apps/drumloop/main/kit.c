/* 808-style drum sounds, synthesized from oscillators, noise and filters
   the way the analog circuits make them, so no sample data is needed. */

#include "kit.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define TWO_PI (2.0f * (float)M_PI)
/* Peak level of every sound before the mix gains. */
#define PEAK 0.89f
/* Fade at the end of each sound so the cut doesn't click. */
#define END_FADE_FRAMES (DRUM_RATE / 200)

static const char *TAG = "kit";

static drum_sound_t sounds[DRUM_TRACKS];
static uint32_t noise_state = 0x6d2b79f5;

/* White noise in -1..1 (xorshift32). */
static float noise(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return (float)(int32_t)noise_state / 2147483648.0f;
}

/* ---- Filters (RBJ biquads) ---------------------------------------------- */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

typedef enum { LOWPASS, HIGHPASS, BANDPASS } filter_type_t;

static void biquad_init(biquad_t *f, filter_type_t type, float freq, float q)
{
    float w0 = TWO_PI * freq / DRUM_RATE;
    float cosw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    switch (type) {
    case LOWPASS:
        f->b0 = (1.0f - cosw) / 2.0f;
        f->b1 = 1.0f - cosw;
        f->b2 = f->b0;
        break;
    case HIGHPASS:
        f->b0 = (1.0f + cosw) / 2.0f;
        f->b1 = -(1.0f + cosw);
        f->b2 = f->b0;
        break;
    case BANDPASS:
        f->b0 = alpha;
        f->b1 = 0.0f;
        f->b2 = -alpha;
        break;
    }
    f->b0 /= a0;
    f->b1 /= a0;
    f->b2 /= a0;
    f->a1 = -2.0f * cosw / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_run(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ---- Voices ------------------------------------------------------------- */

/* Each voice fills buffer[0..frames) with float samples at any level; the
   kit normalizes them afterwards. */

/* Bass drum: a sine swept down from about 160 Hz to 47 Hz with a long
   decay, the trap "808", and a little saturation for punch. */
static void make_kick(float *buffer, uint32_t frames)
{
    float phase = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float freq = 47.0f + 113.0f * expf(-t / 0.03f);
        phase += TWO_PI * freq / DRUM_RATE;
        float x = sinf(phase) * expf(-t / 0.45f);
        x += 0.12f * noise() * expf(-t / 0.002f);
        buffer[i] = tanhf(1.8f * x);
    }
}

/* Snare: two damped tones for the shell and high-passed noise for the
   snares. */
static void make_snare(float *buffer, uint32_t frames)
{
    biquad_t hp;
    biquad_init(&hp, HIGHPASS, 1800.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float tone = 0.5f * sinf(TWO_PI * 185.0f * t) * expf(-t / 0.06f) +
                     0.3f * sinf(TWO_PI * 330.0f * t) * expf(-t / 0.04f);
        float snares = biquad_run(&hp, noise()) * expf(-t / 0.12f);
        buffer[i] = tone + 0.7f * snares;
    }
}

/* Hand clap: band-passed noise in three quick bursts and then a tail. */
static void make_clap(float *buffer, uint32_t frames)
{
    biquad_t bp;
    biquad_init(&bp, BANDPASS, 1100.0f, 1.5f);
    static const float bursts[] = {0.0f, 0.011f, 0.022f};
    const float tail_start = 0.031f;
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float envelope = 0.0f;
        for (int b = 0; b < 3; ++b) {
            if (t >= bursts[b] && t < tail_start) {
                envelope = fmaxf(envelope, expf(-(t - bursts[b]) / 0.004f));
            }
        }
        if (t >= tail_start) {
            envelope = expf(-(t - tail_start) / 0.11f);
        }
        buffer[i] = biquad_run(&bp, noise()) * envelope;
    }
}

/* Hi-hats: six detuned square waves, the 808's metallic source, high-passed
   twice. decay sets closed or open. */
static void make_hat(float *buffer, uint32_t frames, float decay)
{
    static const float freqs[] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    float phases[6] = {0};
    biquad_t hp1;
    biquad_t hp2;
    biquad_init(&hp1, HIGHPASS, 7000.0f, 0.7f);
    biquad_init(&hp2, HIGHPASS, 7000.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float metal = 0.0f;
        for (int o = 0; o < 6; ++o) {
            /* The ratios are scaled up so the partials reach the hat's
               range; the high pass keeps only their upper harmonics. */
            phases[o] += freqs[o] * 1.6f / DRUM_RATE;
            if (phases[o] >= 1.0f) {
                phases[o] -= 1.0f;
            }
            metal += phases[o] < 0.5f ? 1.0f : -1.0f;
        }
        float x = metal / 6.0f + 0.15f * noise();
        x = biquad_run(&hp2, biquad_run(&hp1, x));
        buffer[i] = x * expf(-t / decay);
    }
}

/* Low tom: a sine with a small downward bend. */
static void make_tom(float *buffer, uint32_t frames)
{
    float phase = 0.0f;
    biquad_t lp;
    biquad_init(&lp, LOWPASS, 600.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float freq = 90.0f + 35.0f * expf(-t / 0.05f);
        phase += TWO_PI * freq / DRUM_RATE;
        float x = sinf(phase) * expf(-t / 0.2f);
        x += 0.08f * biquad_run(&lp, noise()) * expf(-t / 0.05f);
        buffer[i] = x;
    }
}

/* Rim shot: two high tones with a very short decay. */
static void make_rim(float *buffer, uint32_t frames)
{
    biquad_t hp;
    biquad_init(&hp, HIGHPASS, 500.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float x = (0.6f * sinf(TWO_PI * 1667.0f * t) + 0.5f * sinf(TWO_PI * 455.0f * t)) *
                  expf(-t / 0.008f);
        buffer[i] = tanhf(2.0f * biquad_run(&hp, x));
    }
}

/* Cowbell: two square waves at 540 and 800 Hz through a band pass, with a
   sharp attack and a softer tail. */
static void make_cowbell(float *buffer, uint32_t frames)
{
    float p1 = 0.0f;
    float p2 = 0.0f;
    biquad_t bp;
    biquad_init(&bp, BANDPASS, 1800.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        p1 += 540.0f / DRUM_RATE;
        p2 += 800.0f / DRUM_RATE;
        p1 -= p1 >= 1.0f ? 1.0f : 0.0f;
        p2 -= p2 >= 1.0f ? 1.0f : 0.0f;
        float x = (p1 < 0.5f ? 1.0f : -1.0f) + (p2 < 0.5f ? 1.0f : -1.0f);
        float envelope = 0.6f * expf(-t / 0.015f) + 0.4f * expf(-t / 0.12f);
        buffer[i] = biquad_run(&bp, x) * envelope;
    }
}

/* ---- Kit ---------------------------------------------------------------- */

typedef struct {
    const char *label;
    float seconds;
    float gain;
} voice_info_t;

static const voice_info_t voice_info[DRUM_TRACKS] = {
    [DRUM_KICK] = {"BD", 1.3f, 0.95f},
    [DRUM_SNARE] = {"SD", 0.35f, 0.7f},
    [DRUM_CLAP] = {"CP", 0.4f, 0.65f},
    [DRUM_CLOSED_HAT] = {"CH", 0.15f, 0.4f},
    [DRUM_OPEN_HAT] = {"OH", 0.8f, 0.4f},
    [DRUM_TOM] = {"LT", 0.6f, 0.65f},
    [DRUM_RIM] = {"RS", 0.07f, 0.5f},
    [DRUM_COWBELL] = {"CB", 0.45f, 0.45f},
};

static void synthesize(int track, float *buffer, uint32_t frames)
{
    switch ((drum_id_t)track) {
    case DRUM_KICK:
        make_kick(buffer, frames);
        break;
    case DRUM_SNARE:
        make_snare(buffer, frames);
        break;
    case DRUM_CLAP:
        make_clap(buffer, frames);
        break;
    case DRUM_CLOSED_HAT:
        make_hat(buffer, frames, 0.025f);
        break;
    case DRUM_OPEN_HAT:
        make_hat(buffer, frames, 0.22f);
        break;
    case DRUM_TOM:
        make_tom(buffer, frames);
        break;
    case DRUM_RIM:
        make_rim(buffer, frames);
        break;
    case DRUM_COWBELL:
        make_cowbell(buffer, frames);
        break;
    }
}

static void *alloc_samples(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(bytes);
}

esp_err_t kit_init(void)
{
    uint32_t longest = 0;
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        uint32_t frames = (uint32_t)(voice_info[track].seconds * DRUM_RATE);
        longest = frames > longest ? frames : longest;
    }
    float *scratch = alloc_samples(longest * sizeof(float));
    if (scratch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t total_bytes = 0;
    for (int track = 0; track < DRUM_TRACKS; ++track) {
        uint32_t frames = (uint32_t)(voice_info[track].seconds * DRUM_RATE);
        int16_t *pcm = alloc_samples(frames * sizeof(int16_t));
        if (pcm == NULL) {
            free(scratch);
            return ESP_ERR_NO_MEM;
        }
        synthesize(track, scratch, frames);
        float peak = 1e-6f;
        for (uint32_t i = 0; i < frames; ++i) {
            peak = fmaxf(peak, fabsf(scratch[i]));
        }
        float scale = PEAK * 32767.0f / peak;
        for (uint32_t i = 0; i < frames; ++i) {
            float fade = 1.0f;
            uint32_t left = frames - i;
            if (left < END_FADE_FRAMES) {
                fade = (float)left / END_FADE_FRAMES;
            }
            pcm[i] = (int16_t)lrintf(scratch[i] * scale * fade);
        }
        sounds[track] = (drum_sound_t){
            .pcm = pcm,
            .frames = frames,
            .label = voice_info[track].label,
            .gain = voice_info[track].gain,
        };
        total_bytes += frames * sizeof(int16_t);
    }
    free(scratch);
    ESP_LOGI(TAG, "Synthesized %d sounds, %u KB", DRUM_TRACKS, (unsigned)(total_bytes / 1024));
    return ESP_OK;
}

const drum_sound_t *kit_sound(int track)
{
    return &sounds[track];
}
