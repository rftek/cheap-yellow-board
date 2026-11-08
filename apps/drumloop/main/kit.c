/* The drum kits: 808, 909, trap and drum and bass sounds, synthesized from
   oscillators, noise and filters the way analog drum machines make them,
   so no sample data is needed. Each kit is the same eight voices with its
   own settings. */

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

static drum_sound_t sounds[DRUM_KITS][DRUM_TRACKS];
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
   kit normalizes them afterwards. p holds the kit's settings for it. */

/* Bass drum: a sine swept down to its pitch (the long "808" of trap), a
   click for the beater and saturation for punch.
   p: pitch Hz, sweep Hz, sweep time s, decay s, click, drive. */
static void make_kick(float *buffer, uint32_t frames, const float *p)
{
    float phase = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float freq = p[0] + p[1] * expf(-t / p[2]);
        phase += TWO_PI * freq / DRUM_RATE;
        float x = sinf(phase) * expf(-t / p[3]);
        x += p[4] * noise() * expf(-t / 0.002f);
        buffer[i] = tanhf(p[5] * x);
    }
}

/* Snare: two damped tones for the shell and high-passed noise for the
   snares. p: tone Hz, second tone Hz, tone decay s, noise high pass Hz,
   noise decay s, noise level. */
static void make_snare(float *buffer, uint32_t frames, const float *p)
{
    biquad_t hp;
    biquad_init(&hp, HIGHPASS, p[3], 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float tone = 0.5f * sinf(TWO_PI * p[0] * t) * expf(-t / p[2]) +
                     0.3f * sinf(TWO_PI * p[1] * t) * expf(-t / (p[2] * 0.67f));
        float snares = biquad_run(&hp, noise()) * expf(-t / p[4]);
        buffer[i] = tone + p[5] * snares;
    }
}

/* Hand clap: band-passed noise in three quick bursts and then a tail.
   p: band Hz, band Q, gap between bursts s, tail decay s. */
static void make_clap(float *buffer, uint32_t frames, const float *p)
{
    biquad_t bp;
    biquad_init(&bp, BANDPASS, p[0], p[1]);
    const float tail_start = p[2] * 2.8f;
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float envelope = 0.0f;
        for (int b = 0; b < 3; ++b) {
            float burst = (float)b * p[2];
            if (t >= burst && t < tail_start) {
                envelope = fmaxf(envelope, expf(-(t - burst) / 0.004f));
            }
        }
        if (t >= tail_start) {
            envelope = expf(-(t - tail_start) / p[3]);
        }
        buffer[i] = biquad_run(&bp, noise()) * envelope;
    }
}

/* Hi-hats and ride: six detuned square waves, the 808's metallic source,
   with some noise, high-passed twice.
   p: decay s, high pass Hz, pitch scale, noise level. */
static void make_metal(float *buffer, uint32_t frames, const float *p)
{
    static const float freqs[] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    float phases[6] = {0};
    biquad_t hp1;
    biquad_t hp2;
    biquad_init(&hp1, HIGHPASS, p[1], 0.7f);
    biquad_init(&hp2, HIGHPASS, p[1], 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float metal = 0.0f;
        for (int o = 0; o < 6; ++o) {
            /* The ratios are scaled up so the partials reach the hat's
               range; the high pass keeps only their upper harmonics. */
            phases[o] += freqs[o] * p[2] / DRUM_RATE;
            if (phases[o] >= 1.0f) {
                phases[o] -= 1.0f;
            }
            metal += phases[o] < 0.5f ? 1.0f : -1.0f;
        }
        float x = metal / 6.0f + p[3] * noise();
        x = biquad_run(&hp2, biquad_run(&hp1, x));
        buffer[i] = x * expf(-t / p[0]);
    }
}

/* Tom: a sine with a small downward bend. p: pitch Hz, bend Hz, decay s. */
static void make_tom(float *buffer, uint32_t frames, const float *p)
{
    float phase = 0.0f;
    biquad_t lp;
    biquad_init(&lp, LOWPASS, 600.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float freq = p[0] + p[1] * expf(-t / 0.05f);
        phase += TWO_PI * freq / DRUM_RATE;
        float x = sinf(phase) * expf(-t / p[2]);
        x += 0.08f * biquad_run(&lp, noise()) * expf(-t / 0.05f);
        buffer[i] = x;
    }
}

/* Rim shot: two high tones with a very short decay.
   p: tone Hz, second tone Hz, decay s. */
static void make_rim(float *buffer, uint32_t frames, const float *p)
{
    biquad_t hp;
    biquad_init(&hp, HIGHPASS, 500.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float x = (0.6f * sinf(TWO_PI * p[0] * t) + 0.5f * sinf(TWO_PI * p[1] * t)) *
                  expf(-t / p[2]);
        buffer[i] = tanhf(2.0f * biquad_run(&hp, x));
    }
}

/* Cowbell: two square waves through a band pass, with a sharp attack and a
   softer tail. p: tone Hz, second tone Hz. */
static void make_cowbell(float *buffer, uint32_t frames, const float *p)
{
    float p1 = 0.0f;
    float p2 = 0.0f;
    biquad_t bp;
    biquad_init(&bp, BANDPASS, 1800.0f, 0.7f);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        p1 += p[0] / DRUM_RATE;
        p2 += p[1] / DRUM_RATE;
        p1 -= p1 >= 1.0f ? 1.0f : 0.0f;
        p2 -= p2 >= 1.0f ? 1.0f : 0.0f;
        float x = (p1 < 0.5f ? 1.0f : -1.0f) + (p2 < 0.5f ? 1.0f : -1.0f);
        float envelope = 0.6f * expf(-t / 0.015f) + 0.4f * expf(-t / 0.12f);
        buffer[i] = biquad_run(&bp, x) * envelope;
    }
}

/* Finger snap: a tight band of noise with a short knock under it.
   p: band Hz, band Q, decay s, knock Hz. */
static void make_snap(float *buffer, uint32_t frames, const float *p)
{
    biquad_t bp;
    biquad_init(&bp, BANDPASS, p[0], p[1]);
    for (uint32_t i = 0; i < frames; ++i) {
        float t = (float)i / DRUM_RATE;
        float x = biquad_run(&bp, noise()) * expf(-t / p[2]);
        x += 0.4f * sinf(TWO_PI * p[3] * t) * expf(-t / 0.006f);
        buffer[i] = x;
    }
}

/* ---- Kits --------------------------------------------------------------- */

typedef void (*voice_fn)(float *buffer, uint32_t frames, const float *p);

typedef struct {
    const char *label; /* two letters */
    voice_fn make;
    float seconds;
    float gain; /* mix level */
    float p[6]; /* the voice's settings */
} sound_def_t;

typedef struct {
    const char *name;
    const char *about;
    sound_def_t sounds[DRUM_TRACKS];
} kit_def_t;

/* Every kit keeps the tracks' roles (kick, snare, clap, closed and open hat,
   tom, rim and one extra), so a beat plays on any kit and the closed hat
   still chokes the open one. */
static const kit_def_t kit_defs[DRUM_KITS] = {
    [KIT_808] = {"808", "CLASSIC", {
        [DRUM_KICK] = {"BD", make_kick, 1.3f, 0.95f, {47, 113, 0.03f, 0.45f, 0.12f, 1.8f}},
        [DRUM_SNARE] = {"SD", make_snare, 0.35f, 0.7f, {185, 330, 0.06f, 1800, 0.12f, 0.7f}},
        [DRUM_CLAP] = {"CP", make_clap, 0.4f, 0.65f, {1100, 1.5f, 0.011f, 0.11f}},
        [DRUM_CLOSED_HAT] = {"CH", make_metal, 0.15f, 0.4f, {0.025f, 7000, 1.6f, 0.15f}},
        [DRUM_OPEN_HAT] = {"OH", make_metal, 0.8f, 0.4f, {0.22f, 7000, 1.6f, 0.15f}},
        [DRUM_TOM] = {"LT", make_tom, 0.6f, 0.65f, {90, 35, 0.2f}},
        [DRUM_RIM] = {"RS", make_rim, 0.07f, 0.5f, {1667, 455, 0.008f}},
        [DRUM_EXTRA] = {"CB", make_cowbell, 0.45f, 0.45f, {540, 800}},
    }},
    [KIT_909] = {"909", "PUNCHY", {
        [DRUM_KICK] = {"BD", make_kick, 0.6f, 0.95f, {52, 200, 0.012f, 0.2f, 0.45f, 2.5f}},
        [DRUM_SNARE] = {"SD", make_snare, 0.4f, 0.7f, {200, 345, 0.05f, 2500, 0.15f, 1.1f}},
        [DRUM_CLAP] = {"CP", make_clap, 0.45f, 0.65f, {1200, 1.2f, 0.010f, 0.14f}},
        [DRUM_CLOSED_HAT] = {"CH", make_metal, 0.18f, 0.4f, {0.035f, 8000, 2.2f, 0.6f}},
        [DRUM_OPEN_HAT] = {"OH", make_metal, 0.9f, 0.4f, {0.3f, 8000, 2.2f, 0.6f}},
        [DRUM_TOM] = {"MT", make_tom, 0.5f, 0.65f, {110, 60, 0.18f}},
        [DRUM_RIM] = {"RS", make_rim, 0.08f, 0.5f, {1700, 500, 0.01f}},
        [DRUM_EXTRA] = {"RD", make_metal, 1.6f, 0.35f, {0.9f, 5000, 1.15f, 0.25f}},
    }},
    [KIT_TRAP] = {"TRAP", "LONG 808 BASS", {
        [DRUM_KICK] = {"BD", make_kick, 2.0f, 1.0f, {44, 140, 0.04f, 0.9f, 0.08f, 3.0f}},
        [DRUM_SNARE] = {"SD", make_snare, 0.25f, 0.7f, {230, 400, 0.03f, 3000, 0.09f, 1.0f}},
        [DRUM_CLAP] = {"CP", make_clap, 0.6f, 0.65f, {1400, 1.0f, 0.009f, 0.2f}},
        [DRUM_CLOSED_HAT] = {"CH", make_metal, 0.08f, 0.35f, {0.012f, 9000, 1.8f, 0.3f}},
        [DRUM_OPEN_HAT] = {"OH", make_metal, 0.4f, 0.35f, {0.14f, 9000, 1.8f, 0.3f}},
        [DRUM_TOM] = {"LT", make_tom, 0.8f, 0.65f, {70, 40, 0.35f}},
        [DRUM_RIM] = {"RS", make_rim, 0.05f, 0.5f, {1900, 520, 0.006f}},
        [DRUM_EXTRA] = {"SN", make_snap, 0.15f, 0.55f, {2200, 2.5f, 0.03f, 900}},
    }},
    [KIT_DNB] = {"DNB", "TIGHT BREAKS", {
        [DRUM_KICK] = {"BD", make_kick, 0.4f, 0.95f, {58, 180, 0.015f, 0.12f, 0.35f, 2.2f}},
        [DRUM_SNARE] = {"SD", make_snare, 0.4f, 0.75f, {245, 420, 0.07f, 2000, 0.18f, 1.2f}},
        [DRUM_CLAP] = {"CP", make_clap, 0.3f, 0.6f, {1300, 1.4f, 0.008f, 0.08f}},
        [DRUM_CLOSED_HAT] = {"CH", make_metal, 0.15f, 0.4f, {0.03f, 6500, 2.0f, 0.8f}},
        [DRUM_OPEN_HAT] = {"OH", make_metal, 1.0f, 0.4f, {0.35f, 6500, 2.0f, 0.8f}},
        [DRUM_TOM] = {"MT", make_tom, 0.4f, 0.65f, {140, 50, 0.12f}},
        [DRUM_RIM] = {"RS", make_rim, 0.08f, 0.5f, {1600, 450, 0.01f}},
        [DRUM_EXTRA] = {"RD", make_metal, 1.6f, 0.35f, {0.9f, 5000, 1.15f, 0.25f}},
    }},
};

static void *alloc_samples(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(bytes);
}

/* Synthesizes one sound into pcm, normalized, with a fade at the end. */
static void render_sound(const sound_def_t *def, float *scratch, int16_t *pcm, uint32_t frames)
{
    def->make(scratch, frames, def->p);
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
}

esp_err_t kit_init(void)
{
    uint32_t longest = 0;
    for (int kit = 0; kit < DRUM_KITS; ++kit) {
        for (int track = 0; track < DRUM_TRACKS; ++track) {
            uint32_t frames = (uint32_t)(kit_defs[kit].sounds[track].seconds * DRUM_RATE);
            longest = frames > longest ? frames : longest;
        }
    }
    float *scratch = alloc_samples(longest * sizeof(float));
    if (scratch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t total_bytes = 0;
    for (int kit = 0; kit < DRUM_KITS; ++kit) {
        for (int track = 0; track < DRUM_TRACKS; ++track) {
            const sound_def_t *def = &kit_defs[kit].sounds[track];
            uint32_t frames = (uint32_t)(def->seconds * DRUM_RATE);
            int16_t *pcm = alloc_samples(frames * sizeof(int16_t));
            if (pcm == NULL) {
                free(scratch);
                return ESP_ERR_NO_MEM;
            }
            render_sound(def, scratch, pcm, frames);
            sounds[kit][track] = (drum_sound_t){
                .pcm = pcm,
                .frames = frames,
                .label = def->label,
                .gain = def->gain,
            };
            total_bytes += frames * sizeof(int16_t);
        }
    }
    free(scratch);
    ESP_LOGI(TAG, "Synthesized %d kits, %u KB", DRUM_KITS, (unsigned)(total_bytes / 1024));
    return ESP_OK;
}

static int clamp_kit(int kit)
{
    return kit >= 0 && kit < DRUM_KITS ? kit : 0;
}

const drum_sound_t *kit_sound(int kit, int track)
{
    return &sounds[clamp_kit(kit)][track];
}

const char *kit_name(int kit)
{
    return kit_defs[clamp_kit(kit)].name;
}

const char *kit_about(int kit)
{
    return kit_defs[clamp_kit(kit)].about;
}
