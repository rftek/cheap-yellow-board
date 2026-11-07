#include "export.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define WAV_HEADER_BYTES 44

static const char *TAG = "export";

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    put_u16(p, (uint16_t)value);
    put_u16(p + 2, (uint16_t)(value >> 16));
}

static void write_wav_header(uint8_t *header, uint32_t frames)
{
    uint32_t data_bytes = frames * sizeof(int16_t);
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_u32(header + 16, 16);                         /* fmt chunk size */
    put_u16(header + 20, 1);                          /* PCM */
    put_u16(header + 22, 1);                          /* mono */
    put_u32(header + 24, DRUM_RATE);                  /* sample rate */
    put_u32(header + 28, DRUM_RATE * sizeof(int16_t)); /* byte rate */
    put_u16(header + 32, sizeof(int16_t));            /* block align */
    put_u16(header + 34, 16);                         /* bits per sample */
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);
}

esp_err_t export_render(const drum_pattern_t *pattern, drum_export_t *out)
{
    memset(out, 0, sizeof(*out));
    drum_pattern_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(snapshot, pattern, sizeof(*snapshot));

    double loop_frames = engine_frames_per_step(snapshot->bpm) * snapshot->bars * DRUM_STEPS;
    uint32_t frames = (uint32_t)ceil(loop_frames) + EXPORT_TAIL_FRAMES;
    size_t size = WAV_HEADER_BYTES + (size_t)frames * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wav == NULL) {
        free(snapshot);
        ESP_LOGW(TAG, "No room for %u KB", (unsigned)(size / 1024));
        return ESP_ERR_NO_MEM;
    }
    write_wav_header(wav, frames);

    /* A separate engine, so the one playing to the speaker is untouched. */
    drum_engine_t engine;
    engine_init(&engine, snapshot, NULL, NULL);
    engine_start(&engine);
    int16_t *pcm = (int16_t *)(wav + WAV_HEADER_BYTES);
    uint32_t loop_whole = frames - EXPORT_TAIL_FRAMES;
    engine_render(&engine, pcm, loop_whole);
    /* Stop before the loop would start again, and let the hits ring out. */
    engine_stop(&engine);
    engine_render(&engine, pcm + loop_whole, EXPORT_TAIL_FRAMES);
    free(snapshot);

    out->wav = wav;
    out->size = size;
    out->frames = frames;
    ESP_LOGI(TAG, "Rendered %u frames (%.1f s, %u KB)", (unsigned)frames,
             (double)frames / DRUM_RATE, (unsigned)(size / 1024));
    return ESP_OK;
}

esp_err_t export_save(const drum_export_t *export, const char *name)
{
    /* TODO: write export->wav somewhere: an SD card, an HTTP download
       served from the board, or an upload to a computer. */
    ESP_LOGW(TAG, "Saving %s (%u bytes) is not supported yet", name, (unsigned)export->size);
    return ESP_ERR_NOT_SUPPORTED;
}

void export_free(drum_export_t *export)
{
    free(export->wav);
    memset(export, 0, sizeof(*export));
}
