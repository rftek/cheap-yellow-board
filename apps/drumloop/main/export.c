#include "export.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdcard.h"

#define WAV_HEADER_BYTES 44
#define WRITE_BYTES 8192

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

esp_err_t export_save(const drum_export_t *export, char *saved, size_t saved_size)
{
    if (sdcard_mount() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    mkdir(EXPORT_DIR, 0775); /* fails harmlessly when it exists */
    char path[64];
    int number = 1;
    struct stat info;
    for (; number <= 999; ++number) {
        snprintf(path, sizeof(path), EXPORT_DIR "/loop-%03d.wav", number);
        if (stat(path, &info) != 0) {
            break;
        }
    }
    if (number > 999) {
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGW(TAG, "Can't create %s", path);
        return ESP_FAIL;
    }
    /* Writes go through internal RAM, which the SD driver can DMA from. */
    uint8_t *buffer = heap_caps_malloc(WRITE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    for (size_t done = 0; done < export->size; done += WRITE_BYTES) {
        size_t take = export->size - done < WRITE_BYTES ? export->size - done : WRITE_BYTES;
        memcpy(buffer, export->wav + done, take);
        if (fwrite(buffer, 1, take, file) != take) {
            result = ESP_FAIL;
            break;
        }
    }
    free(buffer);
    if (fclose(file) != 0) {
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Writing %s failed", path);
        remove(path);
        return result;
    }
    snprintf(saved, saved_size, "loop-%03d.wav", number);
    ESP_LOGI(TAG, "Saved %s (%u KB)", path, (unsigned)(export->size / 1024));
    return ESP_OK;
}

void export_free(drum_export_t *export)
{
    free(export->wav);
    memset(export, 0, sizeof(*export));
}
