# airplay_sender

An ESP-IDF component that plays an app's audio on an AirPlay speaker, like a remote speaker output. It speaks AirPlay 1 (RAOP), which almost every AirPlay speaker still accepts, and was written for this repository from the public protocol descriptions. It contains no code from libraop or other GPL projects.

[apps/airplay_test](../../apps/airplay_test) is the demo.

## Using it

Add the folder to the app's `CMakeLists.txt` and require it from `main`:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_display"
                         "${CMAKE_CURRENT_LIST_DIR}/../../components/airplay_sender")
```

```cmake
idf_component_register(SRCS "main.c" PRIV_REQUIRES airplay_sender ...)
```

The ESP-IDF component manager fetches `espressif/mdns` on the first build. The app joins Wi-Fi itself, then:

| Step | Call | Notes |
|---|---|---|
| 1. Search | `airplay_discover(timeout_ms, devices, max, &count)` | Browses mDNS `_raop._tcp`. Each `airplay_device_t` has the name, IP, port, model and a `supported` flag. Supported speakers come first. |
| 2. Pick a device | `airplay_sender_connect(&device, &config, &handle)` | Opens the RTSP session. `AIRPLAY_SENDER_DEFAULT_CONFIG()` sets 2 s latency, stereo input and volume 50. |
| 3. Send audio | `airplay_sender_write(handle, pcm, frames, timeout, &written)` | 44100 Hz, 16-bit, interleaved, mono or stereo. Blocks while the speaker's buffer is full, like `i2s_channel_write`, so a writer loop is paced in real time. |
| 4. Volume | `airplay_sender_set_volume(handle, 0..100)`, `airplay_sender_get_volume` | Mapped onto the speaker's -30..0 dB range; 0 mutes. |
| 5. Stop audio | `airplay_sender_stop(handle)` | Drops the audio the speaker has buffered, so it goes quiet now; the next write plays again. |
| 6. Disconnect | `airplay_sender_disconnect(handle)` | Ends the session and frees the handle. |

The other calls:

- `airplay_sender_is_connected` and the `on_event` callback (`AIRPLAY_EVENT_DISCONNECTED`) report a speaker that closed the session, for example after it was switched off or taken over by a phone.
- `airplay_sender_latency_ms` returns the delay between a write and the sound, which matters for sound effects that should match the screen. `config.latency_ms` sets it. 2 s is the AirPlay 1 norm. Shairport Sync and many other receivers also play at shorter latencies.

Calls on one handle are not thread-safe against each other. Use one writer task, and take a mutex around stop, volume and disconnect if another task makes those calls (see the demo).

## How it works

- **RTSP over TCP** to the speaker:
  1. `POST /auth-setup`, sent only to Apple's AirPlay 2 speakers (Apple TV and HomePod). It carries a fresh Curve25519 public key from mbedTLS PSA.
  2. `OPTIONS`.
  3. `ANNOUNCE`, with an SDP for Apple Lossless at 352 frames, 16-bit, 2 channels and 44100 Hz.
  4. `SETUP`, which exchanges UDP ports.
  5. `RECORD`.
  6. `SET_PARAMETER` for the volume.
  7. `FLUSH` to stop, and `TEARDOWN` to end.

  `OPTIONS` is also sent every 30 s as a keepalive.
- **Audio**: RTP packets of 352 frames (8 ms) of uncompressed ALAC, sent over UDP. ALAC frames can be stored uncompressed, so no encoder is needed, at about 1.4 Mbit/s. A packet is sent when the sender's clock reaches its RTP timestamp, at most 4 packets early. If the app falls behind by more than half the latency, the timeline jumps forward and the speaker plays the gap as silence.
- **Control**: a sync packet every second ties RTP time to the sender's NTP-style clock and tells the speaker to play each frame `latency` after it was sent. The speaker's resend requests are answered from a backlog of recent packets: 128 by default, about 1 s or 182 KB, kept in PSRAM when there is some.
- **Timing**: the speaker's NTP-style timing requests are answered with the sender's clock, `esp_timer`, so no SNTP is needed.

One network task handles the timing and control sockets and the keepalive, and notices a closed RTSP connection.

## Size

About 10 KB of code, plus about 90 KB for Espressif's mDNS component. At run time it uses the backlog (`backlog_packets` × 1424 bytes) about 4 KB of buffers in the handle and a 6 KB task stack. It runs on the ESP32 as well as the S3; on the CYD, which has no PSRAM, set `backlog_packets` to 32 or so.

## Not supported

- **Password-protected speakers** (TXT `pw=true`). RTSP digest authentication could be added.
- **Speakers that require encrypted audio**, which have no `0` in TXT `et`. The RSA/AES mode (`et=1`) could be added with mbedTLS.
- **Speakers that only accept TCP audio** (TXT `tp` without `UDP`).
- **AirPlay 2-only features**: multi-room sync with PTP timing, HomeKit pairing, and buffered audio.
- **Track metadata and artwork** (DAAP over `SET_PARAMETER`). This is easy to add if an app wants titles on the speaker's display.
- **Other sample rates**: AirPlay 1 is 44100 Hz only, so apps resample.

## Testing status

Played to a Denon AVR-S770H receiver (AirPlay 2, `et=0,4`, so it took the `/auth-setup` path) from the Waveshare S3 on 2026-11-07. Denon Home 250 speakers were found, but haven't been played to yet.
