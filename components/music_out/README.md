# music_out

Sound output for the music apps, [DrumLoop](../../apps/drumloop) and [Keyboard](../../apps/keyboard). It is built on [airplay_sender](../airplay_sender) and [cyd_display](../cyd_display) and has four parts:

- **An audio task** calls the app's render callback for 352 frames (8 ms) at a time, 44.1 kHz mono. While a speaker is attached, it writes every block, silence included, so a note plays without restarting the stream. With no speaker, it calls the callback on the board's clock and throws the audio away, so loops and the screen keep time.
- **Wi-Fi**: it joins the network the launcher saved in NVS (`cyd_config`). Nothing is compiled in.
- **A speaker picker**: a full-screen list of AirPlay speakers, and connect and disconnect. It sends audio one packet ahead (`lead_packets = 1`) so a key or pad is sent sooner.
- **Settings the music apps share**, in NVS namespace `music`: the AirPlay delay asked for at connect (`latency_ms`) and the tempo (`tempo`, in BPM).

```c
static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    my_synth_render(pcm, frames);
    return false;           /* true: drop the speaker's buffer first (a stop) */
}

music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
config.title = " MY APP";   /* heading of the full-screen messages */
config.render = render;
music_out_start(&config, touch_queue);

/* In the app's menu: */
music_out_draw_wifi_row(2);
music_out_draw_output_row(3);
music_out_pick_speaker(status, sizeof(status));
```

Sound follows the screen with a delay of `music_out_latency_frames()`. `music_out_heard_frame()` is the frame the speaker plays now, which an app compares with the frame it rendered something at. DrumLoop's playhead and Keyboard's lit keys work this way.

The API is in [include/music_out.h](include/music_out.h).
