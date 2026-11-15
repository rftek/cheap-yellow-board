#include "wav.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "kit.h"

#define FORMAT_PCM 1
#define FORMAT_FLOAT 3
#define FORMAT_EXTENSIBLE 0xFFFE
#define MAX_CHANNELS 8
#define READ_BYTES 4096
#define END_FADE_FRAMES (DRUM_RATE / 200)

static const char *TAG = "wav";

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;
    uint16_t block_align;
} wav_format_t;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void *alloc_psram(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(bytes);
}

/* Skips a chunk's body, and the pad byte after an odd-sized one. */
static bool skip(FILE *file, uint32_t size)
{
    return fseek(file, (long)size + (long)(size & 1), SEEK_CUR) == 0;
}

/* Finds the fmt and data chunks; leaves the file at the start of the
   audio, with its size in *data_size. */
static esp_err_t read_header(FILE *file, wav_format_t *format, uint32_t *data_size)
{
    uint8_t header[40];
    if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool have_format = false;
    while (fread(header, 1, 8, file) == 8) {
        uint32_t size = get32(header + 4);
        if (memcmp(header, "fmt ", 4) == 0) {
            uint32_t take = size < sizeof(header) ? size : sizeof(header);
            if (take < 16 || fread(header, 1, take, file) != take) {
                return ESP_ERR_INVALID_ARG;
            }
            format->format = get16(header);
            format->channels = get16(header + 2);
            format->rate = get32(header + 4);
            format->block_align = get16(header + 12);
            format->bits = get16(header + 14);
            if (format->format == FORMAT_EXTENSIBLE && take >= 26) {
                /* The sub-format GUID starts with the real format code. */
                format->format = get16(header + 24);
            }
            have_format = true;
            /* The rest of the chunk, and its pad byte if size is odd. */
            if (fseek(file, (long)(size - take) + (long)(size & 1), SEEK_CUR) != 0) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (memcmp(header, "data", 4) == 0) {
            if (!have_format) {
                return ESP_ERR_INVALID_ARG;
            }
            *data_size = size;
            return ESP_OK;
        } else if (!skip(file, size)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static bool format_supported(const wav_format_t *f)
{
    if (f->channels < 1 || f->channels > MAX_CHANNELS || f->rate < 8000 || f->rate > 192000) {
        return false;
    }
    if (f->block_align != f->channels * (f->bits / 8)) {
        return false;
    }
    if (f->format == FORMAT_PCM) {
        return f->bits == 8 || f->bits == 16 || f->bits == 24 || f->bits == 32;
    }
    return f->format == FORMAT_FLOAT && f->bits == 32;
}

/* One frame of the file, mixed to mono, in -1..1. */
static float decode_frame(const wav_format_t *f, const uint8_t *p)
{
    float sum = 0.0f;
    for (int c = 0; c < f->channels; ++c) {
        switch (f->bits) {
        case 8:
            sum += ((int)p[0] - 128) / 128.0f;
            break;
        case 16:
            sum += (int16_t)get16(p) / 32768.0f;
            break;
        case 24: {
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) {
                v -= 0x1000000;
            }
            sum += v / 8388608.0f;
            break;
        }
        default: /* 32 */
            if (f->format == FORMAT_FLOAT) {
                float v;
                memcpy(&v, p, sizeof(v));
                sum += isfinite(v) ? v : 0.0f;
            } else {
                sum += (int32_t)get32(p) / 2147483648.0f;
            }
            break;
        }
        p += f->bits / 8;
    }
    return sum / f->channels;
}

esp_err_t wav_load(const char *path, float max_seconds, float peak, int16_t **pcm,
                   uint32_t *frames)
{
    *pcm = NULL;
    *frames = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    wav_format_t f = {0};
    uint32_t data_size = 0;
    esp_err_t result = read_header(file, &f, &data_size);
    if (result == ESP_OK && !format_supported(&f)) {
        ESP_LOGW(TAG, "%s: format %u, %u bits, %u channels, %lu Hz not supported", path,
                 f.format, f.bits, f.channels, (unsigned long)f.rate);
        result = ESP_ERR_NOT_SUPPORTED;
    }
    if (result != ESP_OK) {
        fclose(file);
        return result;
    }

    uint32_t src_frames = data_size / f.block_align;
    uint32_t src_limit = (uint32_t)(max_seconds * f.rate);
    if (src_frames > src_limit) {
        src_frames = src_limit;
    }
    double step = (double)f.rate / DRUM_RATE; /* source frames per output frame */
    uint32_t out_frames = src_frames > 0 ? (uint32_t)((src_frames - 1) / step) + 1 : 0;
    if (out_frames == 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    float *out = alloc_psram(out_frames * sizeof(float));
    /* Reads go through internal RAM, which the SD driver can DMA into. */
    uint8_t *buffer = heap_caps_malloc(READ_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (out == NULL || buffer == NULL) {
        free(out);
        free(buffer);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    /* Linear interpolation, streamed: output frame j sits at source
       position j * step, between source frames n - 1 and n. */
    uint32_t frames_per_read = READ_BYTES / f.block_align;
    uint32_t n = 0;
    uint32_t done = 0;
    double next_pos = 0.0;
    float previous = 0.0f;
    while (n < src_frames && done < out_frames) {
        uint32_t want = src_frames - n < frames_per_read ? src_frames - n : frames_per_read;
        size_t got = fread(buffer, f.block_align, want, file);
        if (got == 0) {
            break; /* a short file: keep what was read */
        }
        for (size_t i = 0; i < got && done < out_frames; ++i, ++n) {
            float current = decode_frame(&f, buffer + i * f.block_align);
            while (next_pos <= (double)n && done < out_frames) {
                float frac = (float)(next_pos - ((double)n - 1.0));
                out[done] = n == 0 ? current : previous + (current - previous) * frac;
                ++done;
                next_pos = done * step;
            }
            previous = current;
        }
    }
    free(buffer);
    fclose(file);
    if (done == 0) {
        free(out);
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t *samples = alloc_psram(done * sizeof(int16_t));
    if (samples == NULL) {
        free(out);
        return ESP_ERR_NO_MEM;
    }
    float loudest = 1e-6f;
    for (uint32_t i = 0; i < done; ++i) {
        loudest = fmaxf(loudest, fabsf(out[i]));
    }
    float scale = peak * 32767.0f / loudest;
    for (uint32_t i = 0; i < done; ++i) {
        float fade = 1.0f;
        uint32_t left = done - i;
        if (left < END_FADE_FRAMES) {
            fade = (float)left / END_FADE_FRAMES;
        }
        samples[i] = (int16_t)lrintf(out[i] * scale * fade);
    }
    free(out);
    *pcm = samples;
    *frames = done;
    return ESP_OK;
}
