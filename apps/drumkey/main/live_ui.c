/* Drum&Key's LIVE screen: LiveDrum's pads, sound picker, loop recorder and
   banks (apps/livedrum/main/ld_ui.c), on Drum&Key's shared transport,
   tempo and speaker. Its pads can also play the SYNTH screen's samples.
   The audio task runs LiveDrum's engine (dk_audio.c). */

#include "dk_audio.h"
#include "dk_ui.h"
#include "ld_sounds.h"
#include "ld_ui.h"
#include "psynth.h"

/* The loops start together from PLAY on any screen, so no count-in. */
static void live_play(bool play, int count_in)
{
    (void)count_in;
    dk_play(play);
}

void live_screen(QueueHandle_t touch_queue)
{
    static const ld_synth_source_t synth = {
        .count = PSYNTH_SAMPLES,
        .name = psynth_sample_name,
        .pcm = psynth_sample_pcm,
    };
    static const ld_host_t host = {
        .title = " DRUM&KEY: LIVE SETTINGS",
        .play = live_play,
        .is_playing = dk_is_playing,
        .tempo = dk_tempo,
        .set_tempo = dk_set_tempo,
        .switch_label = "ROLL",
        .switch_color = DK_ROLL_COLOR,
        .has_count_in = false,
        .status = dk_status_text,
    };
    ld_sounds_set_synth(&synth);
    ld_screen(touch_queue, &host);
}
