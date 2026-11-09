#pragma once

/* Sound output for the music apps (DrumLoop, Keyboard): Wi-Fi, a speaker
   picker, and an audio task that streams the app's sound to an AirPlay
   speaker through airplay_sender, plus the settings those apps share.

   The app renders 44100 Hz mono 16-bit audio in a callback, 8 ms at a
   time. While a speaker is attached the task always writes, silence
   included, so a note plays without restarting the stream. With no
   speaker (silent, the default) it calls the callback on the board's
   clock and throws the audio away, so loops and the screen keep time.

   Typical use:

       music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
       config.title = " KEYBOARD";
       config.render = render;               // the app's synth or sequencer
       music_out_start(&config, touch_queue);
       ...
       music_out_pick_speaker(status, sizeof(status));   // from a menu

   Nothing is compiled in about the network: Wi-Fi uses the network the
   launcher saved in NVS (cyd_config). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_sender.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_OUT_RATE AIRPLAY_SAMPLE_RATE
#define MUSIC_OUT_BLOCK_FRAMES AIRPLAY_FRAMES_PER_PACKET /* 352, 8 ms */

/* Renders frames of mono audio into pcm. Runs in the audio task, 8 ms
   blocks, at a priority above the UI. Return true to drop what the
   speaker has buffered first, so a stop is heard at once instead of after
   the AirPlay delay. speaker_attached is false in silent mode. */
typedef bool (*music_out_render_fn)(int16_t *pcm, size_t frames, bool speaker_attached,
                                    void *context);

/* Called in the audio task when the output changes, before the next
   block: the latency may have changed. */
typedef void (*music_out_changed_fn)(void *context);

typedef struct {
    const char *title; /* the app's name for full-screen messages, " DRUMLOOP" */
    music_out_render_fn render;
    music_out_changed_fn on_change; /* may be NULL */
    void *context;
    int task_priority; /* above the UI task, so drawing never delays a packet */
    uint32_t task_stack;
    int task_core; /* 0 or 1 to pin the audio task to a core, -1 for either */
} music_out_config_t;

#define MUSIC_OUT_DEFAULT_CONFIG() \
    {                              \
        .title = " MUSIC",         \
        .render = NULL,            \
        .on_change = NULL,         \
        .context = NULL,           \
        .task_priority = 7,        \
        .task_stack = 6144,        \
        .task_core = -1,           \
    }

/* Reads the saved Wi-Fi network and starts the audio task, silent. The
   picker reads taps from touch_queue (cyd_touch_start's). NVS must be
   initialized. */
esp_err_t music_out_start(const music_out_config_t *config, QueueHandle_t touch_queue);

/* ---- Audio -------------------------------------------------------------- */

bool music_out_has_speaker(void);

/* Volume 0-100 on the attached speaker. */
esp_err_t music_out_set_volume(int volume);
int music_out_get_volume(void);

/* Frames rendered since start (low 32 bits), counting silent mode too. */
uint32_t music_out_frames_written(void);

/* How far the speaker plays behind what is rendered, in frames; 0 when
   silent. A note rendered now is heard this many frames later. */
uint32_t music_out_latency_frames(void);

/* The frame the speaker is playing now: frames written minus latency. */
uint32_t music_out_heard_frame(void);

/* ---- Wi-Fi -------------------------------------------------------------- */

bool music_out_wifi_saved(void);  /* the launcher has a network saved */
bool music_out_wifi_on(void);     /* the radio is on */
bool music_out_wifi_joined(void); /* on and joined */
const char *music_out_wifi_ssid(void);

/* Turns the radio on and joins the saved network, showing progress on a
   full screen (the caller redraws afterwards). On failure the radio stays
   on and status says why. */
bool music_out_wifi_start(char *status, size_t status_size);

/* Disconnects the speaker, if any, and turns the radio off. */
void music_out_wifi_stop(void);

/* ---- Speaker ------------------------------------------------------------ */

/* Full-screen picker: joins Wi-Fi if needed, searches for AirPlay speakers
   and connects to the one tapped (with the saved AirPlay delay), or turns
   the speaker off. Takes one-finger taps: turn the touch driver's fast
   mode off first. The caller redraws afterwards; status gets a line about
   the outcome. */
void music_out_pick_speaker(char *status, size_t status_size);

/* The speaker's name, or NULL when silent. */
const char *music_out_speaker_name(void);

/* Call often: when the speaker has dropped the session, disconnects it,
   goes silent and returns true (once). */
bool music_out_check_lost(void);

/* ---- Menu rows ---------------------------------------------------------- */

/* One full-width text row each, as in DrumLoop's and Keyboard's menus. */
void music_out_draw_wifi_row(int row);
void music_out_draw_output_row(int row);
void music_out_draw_latency_row(int row);

/* WI-FI row tapped: turns Wi-Fi off, or on (full screen). Returns true
   when the caller must redraw its whole menu. status gets a line. */
bool music_out_tap_wifi(char *status, size_t status_size);

/* ---- Settings shared by the music apps (NVS namespace "music") ---------- */

/* The AirPlay delay asked for at the next connect, in ms. 2000 ms is the
   AirPlay 1 norm; shorter makes playing live feel more immediate, but some
   speakers stay silent below their own minimum (a Denon Home 250 needs
   500). The speaker may ask for more. */
#define MUSIC_OUT_DEFAULT_LATENCY_MS 2000
uint32_t music_out_latency_setting_ms(void);

/* Steps to the next delay of 2000, 1000, 500, 250, 150 and 100 ms, saves
   it and returns it. */
uint32_t music_out_next_latency_setting(void);

/* The tempo the music apps share, so a keyboard part and a drum loop made
   one after the other match. */
#define MUSIC_TEMPO_MIN 60
#define MUSIC_TEMPO_MAX 200
uint16_t music_out_load_tempo(uint16_t fallback);
void music_out_save_tempo(uint16_t bpm);

#ifdef __cplusplus
}
#endif
