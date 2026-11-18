#pragma once

/* LiveDrum's sound: sixteen pads, each a sample with a level, a pitch and a
   choke group, and a loop recorder that takes the pads' hits as they are
   played and plays them back, overdubbed take on take.

   A pad is played from the touch task (ld_hit) and sounds at the start of
   the next audio block, at most 8 ms later. The host app's audio task runs
   the engine: ld_engine_render each block, ld_engine_start / _stop for the
   transport. Everything else is called from the UI task.

   The loop is 1, 2, 4 or 8 bars of 4/4. Hits are kept in ticks (96 to a
   beat), so the tempo can change and the loop follows. They are recorded
   where the player heard the loop when they hit the pad: the render
   position minus the speaker's delay (music_out_latency_frames), then
   moved to the nearest quantize step. Each REC, from on to off, is a take
   that UNDO removes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LD_RATE 44100
#define LD_PADS 16
#define LD_TICKS_PER_BEAT 96
#define LD_BEATS_PER_BAR 4
#define LD_TICKS_PER_BAR (LD_TICKS_PER_BEAT * LD_BEATS_PER_BAR)
#define LD_MAX_BARS 8
#define LD_MAX_HITS 2048
#define LD_LEVEL_MAX 100
#define LD_LEVEL_DEFAULT 80
#define LD_PITCH_MIN (-12)
#define LD_PITCH_MAX 12
#define LD_CHOKE_GROUPS 3 /* 0 is none; a hit cuts the other pads in its group */

typedef enum {
    LD_QUANTIZE_OFF,
    LD_QUANTIZE_8,
    LD_QUANTIZE_16,
    LD_QUANTIZE_32,
    LD_QUANTIZE_8T,
    LD_QUANTIZE_16T,
    LD_QUANTIZE_MODES,
} ld_quantize_t;

/* A pad's sound: 44100 Hz mono 16-bit. Whoever owns pcm must not free it
   while a pad holds it; ld_set_sample says when it is let go. */
typedef struct {
    const int16_t *pcm;
    uint32_t frames;
    float gain; /* mix level of the sound itself, before the pad's level */
} ld_sample_t;

/* What the UI sets directly; the next hit uses it. */
typedef struct {
    volatile uint8_t level; /* 0-100 */
    volatile int8_t pitch;  /* semitones, LD_PITCH_MIN..MAX */
    volatile uint8_t choke; /* 0, or a group 1..LD_CHOKE_GROUPS */
} ld_pad_settings_t;

/* One recorded hit. */
typedef struct {
    uint16_t tick; /* from the start of the loop */
    uint8_t pad;
    uint8_t take;
    uint8_t skip; /* recorded just ahead of the render: let it pass once */
    uint8_t reserved;
} ld_loop_hit_t;

/* Things the speaker has reached, for the screen: a loop hit to light, or
   a beat to show. */
typedef enum {
    LD_HEARD_PAD,
    LD_HEARD_BEAT,
} ld_heard_type_t;

typedef struct {
    uint32_t frame; /* stream frame it was rendered at (low 32 bits) */
    uint8_t type;
    uint8_t pad;   /* LD_HEARD_PAD */
    int16_t beat;  /* LD_HEARD_BEAT: beat in the loop, negative in the count-in */
} ld_heard_t;

/* Settings the audio task reads each block. */
extern volatile bool ld_metronome;
extern volatile uint8_t ld_quantize; /* ld_quantize_t */

esp_err_t ld_engine_init(void);

/* ---- From the touch and UI tasks ------------------------------------------ */

/* Plays a pad at the start of the next block; while recording, it is also
   written into the loop. Safe from any task, never blocks. */
void ld_hit(int pad);

ld_pad_settings_t *ld_pad(int pad);

/* Gives a pad a new sound (NULL or no frames: silent). Returns once the
   audio task has stopped every voice still playing the old one, so the
   caller may free it then. */
void ld_set_sample(int pad, const ld_sample_t *sample);
const ld_sample_t *ld_sample(int pad);

void ld_record(bool on);
bool ld_is_recording(void);

/* Removes the last take (and ends recording); false when there is none. */
bool ld_undo(void);
void ld_clear(void);

/* 1 -> 2 -> 4 -> 8 bars, copying the hits into the new bars; from 8 back to
   1, dropping the hits after bar 1. */
void ld_cycle_bars(void);
int ld_bars(void);

int ld_hit_count(void);
int ld_take_count(void);

/* Copies the loop out (up to max hits) and returns how many; or replaces
   it, for the banks. */
int ld_get_loop(ld_loop_hit_t *hits, int max, int *bars);
void ld_set_loop(const ld_loop_hit_t *hits, int count, int bars);

/* The next thing the speaker has now played, if any. */
bool ld_next_heard(ld_heard_t *heard);

/* ---- In the host's audio task ---------------------------------------------- */

/* Starts the loop from bar 1, after count_in bars of clicks. */
void ld_engine_start(int count_in);

/* Stops the loop, ends any take and cuts every sound. */
void ld_engine_stop(void);

bool ld_engine_playing(void);

/* Renders frames of mono audio at bpm. first_frame is the stream frame of
   pcm[0] (music_out_frames_written at the block's start). */
void ld_engine_render(int16_t *pcm, size_t frames, uint16_t bpm, uint32_t first_frame);
