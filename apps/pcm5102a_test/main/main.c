/* PCM5102A test: low-latency sound from the Waveshare ESP32-S3-Touch-LCD-7
   through TI's PCM5102A DAC over I2S. Four drum pads play on touch-down; a
   test tone or sweep runs underneath. The screen shows the clocks, the
   delay from a tap to the line output and any underruns, and has buttons
   for the DMA buffer size and the sample rate. The wiring is in README.md. */

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cyd_display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pcm5102a.h"

static const char *TAG = "pcm5102a_test";

#define APP_VERSION "1.0"

/* Set to 1 after bridging the DAC module's FLT jumper (H1) to H, the
   low-latency filter. Only the latency readout uses it. */
#define DAC_LOW_LATENCY_FILTER 0

/* GPIO44 is both the UART2 header's RXD and, through the UART switch SW1,
   the CH343's TXD. The CH343 holds its TXD high, so with a pull-down a high
   reading means SW1 is on UART1 and driving the pin would fight it. */
#define SWITCH_SENSE_GPIO GPIO_NUM_44
#define SENSE_EVERY_US 500000

#define MAX_BLOCK_FRAMES 256
#define PAD_COUNT 4
#define PAD_LEVEL 0.5f /* -6 dBFS peaks */
#define FADE_S 0.002f  /* every voice fades out over its last 2 ms */
#define SWEEP_S 10.0f  /* 20 Hz to 20 kHz */

/* Screen: 800x480, 12 text rows of 40 pixels, 26 columns of 30. */
#define TITLE_ROW 0
#define STATUS_ROW 1
#define INFO_Y 80 /* four small-font lines, 20 pixels each */
#define INFO_LINES 4
#define INFO_COLUMNS 52
#define PAD_Y 160
#define PAD_HEIGHT 200
#define PAD_WIDTH (CYD_LCD_WIDTH / PAD_COUNT)
#define BUTTON_ROW_A 9
#define BUTTON_ROW_B 10
#define HINT_Y 450
#define BUTTON_CELLS 8 /* three buttons of 8 cells with 1-cell gaps */
#define FLASH_US 120000

#define COLOR_GREEN 0x07E0
#define COLOR_TITLE 0x0010
#define COLOR_PAD 0x18E3
#define COLOR_PAD_HIT 0xFD20

/* ---- Settings, set by the UI task and read by the audio task ---- */

typedef enum { TONE_OFF, TONE_440, TONE_1K, TONE_SWEEP, TONE_COUNT } tone_t;
typedef enum { OUT_BOTH, OUT_LEFT, OUT_RIGHT, OUT_COUNT } out_t;

static const char *const tone_labels[] = {"TONE OFF", " 440 Hz", " 1 kHz", " SWEEP"};
static const int level_db[] = {-20, -12, -6, 0};
static const char *const out_labels[] = {" L + R", " LEFT", " RIGHT"};
static const uint16_t buffer_frames[] = {32, 64, 128, 256};
static const uint32_t rates[] = {44100, 48000, 96000};
static const char *const rate_labels[] = {"44.1 kHz", " 48 kHz", " 96 kHz"};
static const char *const pad_names[PAD_COUNT] = {"KICK", "SNARE", "HAT", "BEEP 1k"};

#define COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))
#define DMA_BUFFERS 3

static volatile int tone_index;
static volatile int level_index = 1; /* -12 dB */
static volatile int out_index;
static volatile int buffer_index = 1; /* 64 frames */
static volatile int rate_index;
static volatile bool want_playing;

/* ---- State, set by the audio task and read by the UI task ---- */

static volatile bool dac_open;
static volatile esp_err_t open_error = ESP_OK;
static volatile uint32_t playing_rate;
static volatile uint16_t playing_frames;
static volatile uint32_t underruns;
static volatile uint32_t write_errors;
static volatile int last_pad = -1;
static volatile uint32_t last_pickup_us; /* from the tap to the block that plays it */
static volatile uint32_t pad_hits[PAD_COUNT];

/* Pads tapped and not yet started, one bit each, and when. */
static atomic_uint pads_pending;
static volatile int64_t pad_tap_time[PAD_COUNT];

/* ---- Synthesis ---- */

typedef struct {
    uint32_t left;  /* samples to go, 0 when silent */
    uint32_t fade;  /* samples in the fade-out */
    float env;
    float decay;    /* env multiplier per sample */
    float phase;    /* 0..1 */
    float sweep;    /* kick: Hz above its 50 Hz base, shrinking */
    float sweep_decay;
    float last_noise;
} voice_t;

static voice_t voices[PAD_COUNT];
static uint32_t noise_state = 0x12345678;
static float tone_phase;
static float sweep_time;

static float noise(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return (float)(int32_t)noise_state / 2147483648.0f;
}

static float decay_per_sample(float seconds, float rate)
{
    return expf(-1.0f / (seconds * rate));
}

static void start_voice(int pad, float rate)
{
    static const float lengths[PAD_COUNT] = {0.5f, 0.35f, 0.15f, 0.04f};
    static const float decays[PAD_COUNT] = {0.15f, 0.07f, 0.02f, 100.0f};
    voice_t *voice = &voices[pad];
    voice->left = (uint32_t)(lengths[pad] * rate);
    voice->fade = (uint32_t)(FADE_S * rate);
    voice->env = 1.0f;
    voice->decay = decay_per_sample(decays[pad], rate);
    voice->phase = 0.0f;
    voice->sweep = 110.0f;
    voice->sweep_decay = decay_per_sample(0.03f, rate);
    voice->last_noise = 0.0f;
}

static float voice_sample(int pad, float rate)
{
    voice_t *voice = &voices[pad];
    if (voice->left == 0) {
        return 0.0f;
    }
    float value = 0.0f;
    switch (pad) {
    case 0: /* kick: a sine falling from 160 Hz to 50 Hz */
        value = sinf(2.0f * (float)M_PI * voice->phase);
        voice->phase += (50.0f + voice->sweep) / rate;
        voice->sweep *= voice->sweep_decay;
        break;
    case 1: /* snare: noise over a 185 Hz body */
        value = 0.65f * noise() + 0.35f * sinf(2.0f * (float)M_PI * voice->phase);
        voice->phase += 185.0f / rate;
        break;
    case 2: { /* hat: high-passed noise */
        float white = noise();
        value = 0.5f * (white - voice->last_noise);
        voice->last_noise = white;
        break;
    }
    default: /* beep: 40 ms at 1 kHz, a sharp start for timing */
        value = sinf(2.0f * (float)M_PI * voice->phase);
        voice->phase += 1000.0f / rate;
        break;
    }
    if (voice->phase >= 1.0f) {
        voice->phase -= 1.0f;
    }
    float gain = voice->env;
    if (voice->left < voice->fade) {
        gain *= (float)voice->left / (float)voice->fade;
    }
    voice->env *= voice->decay;
    voice->left--;
    return value * gain * PAD_LEVEL;
}

static void render(int16_t *pcm, size_t frames, float rate)
{
    tone_t tone = (tone_t)tone_index;
    float amplitude = powf(10.0f, (float)level_db[level_index] / 20.0f);
    float frequency = tone == TONE_440 ? 440.0f : 1000.0f;
    if (tone == TONE_SWEEP) {
        frequency = 20.0f * powf(1000.0f, sweep_time / SWEEP_S);
        sweep_time += (float)frames / rate;
        if (sweep_time >= SWEEP_S) {
            sweep_time -= SWEEP_S;
        }
    }
    out_t out = (out_t)out_index;
    for (size_t i = 0; i < frames; ++i) {
        float mix = 0.0f;
        if (tone != TONE_OFF) {
            mix = amplitude * sinf(2.0f * (float)M_PI * tone_phase);
            tone_phase += frequency / rate;
            if (tone_phase >= 1.0f) {
                tone_phase -= 1.0f;
            }
        }
        for (int pad = 0; pad < PAD_COUNT; ++pad) {
            mix += voice_sample(pad, rate);
        }
        if (mix > 1.0f) {
            mix = 1.0f;
        } else if (mix < -1.0f) {
            mix = -1.0f;
        }
        int16_t sample = (int16_t)(mix * 32767.0f);
        pcm[i * 2] = out == OUT_RIGHT ? 0 : sample;
        pcm[i * 2 + 1] = out == OUT_LEFT ? 0 : sample;
    }
}

/* ---- Audio task: owns the DAC; opens, reconfigures and closes it as the
   settings ask, and always writes, silence included ---- */

static void audio_task(void *argument)
{
    (void)argument;
    static int16_t block[MAX_BLOCK_FRAMES * 2];
    pcm5102a_handle_t dac = NULL;

    for (;;) {
        if (!want_playing || (dac != NULL && buffer_frames[buffer_index] != playing_frames)) {
            if (dac != NULL) {
                memset(block, 0, sizeof(block));
                pcm5102a_write(dac, block, playing_frames, 2, 100); /* end on silence */
                pcm5102a_close(dac);
                dac = NULL;
                dac_open = false;
                ESP_LOGI(TAG, "DAC closed");
            }
            if (!want_playing) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        if (dac == NULL) {
            pcm5102a_config_t config = PCM5102A_WAVESHARE_S3_CONFIG();
            config.sample_rate_hz = rates[rate_index];
            config.dma_buffers = DMA_BUFFERS;
            config.dma_frames = buffer_frames[buffer_index];
            config.low_latency_filter = DAC_LOW_LATENCY_FILTER;
            esp_err_t err = pcm5102a_open(&config, &dac);
            if (err != ESP_OK) {
                open_error = err;
                want_playing = false;
                continue;
            }
            open_error = ESP_OK;
            playing_rate = config.sample_rate_hz;
            playing_frames = config.dma_frames;
            dac_open = true;
        }
        if (rates[rate_index] != playing_rate) {
            if (pcm5102a_set_sample_rate(dac, rates[rate_index]) == ESP_OK) {
                playing_rate = rates[rate_index];
            } else {
                write_errors++;
            }
        }

        float rate = (float)playing_rate;
        unsigned pending = atomic_exchange(&pads_pending, 0);
        if (pending != 0) {
            int64_t now = esp_timer_get_time();
            for (int pad = 0; pad < PAD_COUNT; ++pad) {
                if (pending & (1u << pad)) {
                    start_voice(pad, rate);
                    last_pickup_us = (uint32_t)(now - pad_tap_time[pad]);
                    last_pad = pad;
                    pad_hits[pad]++;
                }
            }
        }
        render(block, playing_frames, rate);
        if (pcm5102a_write(dac, block, playing_frames, 2, 100) != ESP_OK) {
            write_errors++;
        }
        underruns = pcm5102a_underruns(dac);
    }
}

/* ---- Touch: pads play from the touch task, on touch-down ---- */

static int pad_at(const cyd_touch_point_t *point)
{
    if (point->y < PAD_Y || point->y >= PAD_Y + PAD_HEIGHT || point->x < 0) {
        return -1;
    }
    int pad = point->x / PAD_WIDTH;
    return pad < PAD_COUNT ? pad : -1;
}

static bool on_tap(const cyd_touch_point_t *point, void *context)
{
    (void)context;
    int pad = pad_at(point);
    if (pad < 0) {
        return false;
    }
    pad_tap_time[pad] = esp_timer_get_time();
    atomic_fetch_or(&pads_pending, 1u << pad);
    return true;
}

/* ---- Screen ---- */

/* Reads GPIO44 with the internal pull-down. Only while the DAC is closed. */
static bool switch_on_header(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << SWITCH_SENSE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    esp_rom_delay_us(200);
    int level = gpio_get_level(SWITCH_SENSE_GPIO);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&config);
    return level == 0;
}

/* A text row redrawn only when its text or color changes. */
typedef struct {
    char text[INFO_COLUMNS + 1];
    uint16_t color;
    bool drawn;
} line_cache_t;

static line_cache_t row_cache[CYD_TEXT_ROWS];
static line_cache_t info_cache[INFO_LINES];

static void text_row(int row, uint16_t foreground, uint16_t background, const char *text)
{
    line_cache_t *cache = &row_cache[row];
    if (cache->drawn && cache->color == foreground && strcmp(cache->text, text) == 0) {
        return;
    }
    cyd_text_row(row, foreground, background, text);
    strlcpy(cache->text, text, sizeof(cache->text));
    cache->color = foreground;
    cache->drawn = true;
}

/* One small-font line under the status row, padded so a shorter text
   covers a longer one. */
static void info_line(int line, uint16_t color, const char *format, ...)
{
    char text[INFO_COLUMNS + 1];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) {
        length = 0;
    }
    for (int i = length; i < INFO_COLUMNS; ++i) {
        text[i] = ' ';
    }
    text[INFO_COLUMNS] = '\0';
    line_cache_t *cache = &info_cache[line];
    if (cache->drawn && cache->color == color && strcmp(cache->text, text) == 0) {
        return;
    }
    cyd_draw_small_text(10, INFO_Y + line * CYD_SMALL_FONT_HEIGHT, text, color, CYD_COLOR_BLACK);
    strlcpy(cache->text, text, sizeof(cache->text));
    cache->color = color;
    cache->drawn = true;
}

static void button_row(int row, const char *first, const char *second, const char *third)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), "%-8.8s %-8.8s %-8.8s", first, second, third);
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        bool gap = i % (BUTTON_CELLS + 1) == BUTTON_CELLS;
        foreground[i] = CYD_COLOR_WHITE;
        background[i] = gap ? CYD_COLOR_BLACK : CYD_COLOR_BUTTON;
    }
    cyd_draw_cells(row, line, foreground, background);
}

static bool user_stopped;

static void draw_buttons(void)
{
    char level[16];
    char buffer[16];
    snprintf(level, sizeof(level), " %d dB", level_db[level_index]);
    snprintf(buffer, sizeof(buffer), " BUF %d", buffer_frames[buffer_index]);
    button_row(BUTTON_ROW_A, tone_labels[tone_index], level, out_labels[out_index]);
    button_row(BUTTON_ROW_B, buffer, rate_labels[rate_index], want_playing ? " STOP" : " START");
}

static void draw_pad(int pad, bool hit)
{
    uint16_t color = hit ? COLOR_PAD_HIT : COLOR_PAD;
    int x = pad * PAD_WIDTH + 5;
    cyd_fill_rect(x, PAD_Y + 5, PAD_WIDTH - 10, PAD_HEIGHT - 10, color);
    const char *name = pad_names[pad];
    int width = (int)strlen(name) * CYD_SMALL_FONT_WIDTH;
    cyd_draw_small_text(pad * PAD_WIDTH + (PAD_WIDTH - width) / 2,
                        PAD_Y + (PAD_HEIGHT - CYD_SMALL_FONT_HEIGHT) / 2, name, CYD_COLOR_WHITE, color);
}

static void handle_tap(const cyd_touch_point_t *point)
{
    int row = point->y / CYD_FONT_HEIGHT;
    if (row != BUTTON_ROW_A && row != BUTTON_ROW_B) {
        return;
    }
    int cell = point->x / CYD_FONT_WIDTH;
    if (cell % (BUTTON_CELLS + 1) == BUTTON_CELLS) {
        return; /* the gap between buttons */
    }
    int button = cell / (BUTTON_CELLS + 1);
    if (row == BUTTON_ROW_A) {
        if (button == 0) {
            tone_index = (tone_index + 1) % TONE_COUNT;
        } else if (button == 1) {
            level_index = (level_index + 1) % COUNT(level_db);
        } else {
            out_index = (out_index + 1) % OUT_COUNT;
        }
    } else {
        if (button == 0) {
            buffer_index = (buffer_index + 1) % COUNT(buffer_frames);
        } else if (button == 1) {
            rate_index = (rate_index + 1) % COUNT(rates);
        } else if (want_playing) {
            want_playing = false;
            user_stopped = true;
        } else {
            /* START plays even if the switch reads as UART1: the reading
               can be wrong if the DAC board pulls GPIO44 up. */
            user_stopped = false;
            want_playing = true;
        }
    }
    draw_buttons();
}

static void ui_loop(QueueHandle_t taps)
{
    char line[CYD_TEXT_COLUMNS + 1];
    uint32_t seen_hits[PAD_COUNT] = {0};
    int64_t lit_until[PAD_COUNT] = {0};
    int64_t next_sense = 0;
    bool switch_ok = false;
    bool drawn_playing = false;

    for (;;) {
        cyd_touch_point_t point;
        while (xQueueReceive(taps, &point, 0) == pdTRUE) {
            handle_tap(&point);
        }
        int64_t now = esp_timer_get_time();

        /* Start on its own once SW1 is on UART2, unless stopped by hand. */
        if (!want_playing && !dac_open && now >= next_sense) {
            next_sense = now + SENSE_EVERY_US;
            switch_ok = switch_on_header();
            if (switch_ok && !user_stopped && open_error == ESP_OK) {
                want_playing = true;
            }
        }
        if (drawn_playing != want_playing) {
            drawn_playing = want_playing;
            draw_buttons();
        }

        uint32_t rate = playing_rate;
        uint16_t frames = playing_frames;
        if (dac_open) {
            snprintf(line, sizeof(line), "PLAYING %.1f kHz", rate / 1000.0f);
            text_row(STATUS_ROW, COLOR_GREEN, CYD_COLOR_BLACK, line);
        } else if (open_error != ESP_OK) {
            snprintf(line, sizeof(line), "I2S: %s", esp_err_to_name(open_error));
            text_row(STATUS_ROW, CYD_COLOR_RED, CYD_COLOR_BLACK, line);
        } else if (user_stopped) {
            text_row(STATUS_ROW, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, "STOPPED");
        } else if (!switch_ok) {
            text_row(STATUS_ROW, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, "SET SW1 TO UART2");
        } else {
            text_row(STATUS_ROW, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, "STARTING");
        }

        float filter_samples = DAC_LOW_LATENCY_FILTER ? 3.5f : 20.0f;
        if (dac_open && rate > 0) {
            float queue_ms = DMA_BUFFERS * frames * 1000.0f / rate;
            float filter_ms = filter_samples * 1000.0f / rate;
            info_line(0, CYD_COLOR_WHITE, "BCK %.3f MHz  LRCK %.1f kHz  64 fs, SCK at GND",
                      rate * 64 / 1e6f, rate / 1000.0f);
            info_line(1, CYD_COLOR_WHITE, "QUEUE %d x %u = %.2f ms  FILTER %s %.2f ms", DMA_BUFFERS,
                      frames, queue_ms, DAC_LOW_LATENCY_FILTER ? "LOW" : "NORMAL", filter_ms);
            int pad = last_pad;
            if (pad >= 0) {
                float pickup_ms = last_pickup_us / 1000.0f;
                info_line(2, CYD_COLOR_CYAN, "%s: PICKUP %.2f + OUT %.2f = %.2f ms", pad_names[pad],
                          pickup_ms, queue_ms + filter_ms, pickup_ms + queue_ms + filter_ms);
            } else {
                info_line(2, CYD_COLOR_GRAY, "TAP A PAD: TAP-TO-OUTPUT DELAY SHOWS HERE");
            }
            uint32_t lost = underruns;
            uint32_t errors = write_errors;
            info_line(3, lost || errors ? CYD_COLOR_RED : CYD_COLOR_GRAY,
                      "UNDERRUNS %lu  ERRORS %lu  (TOUCH SCAN ADDS 5-10 ms)", (unsigned long)lost,
                      (unsigned long)errors);
        } else {
            info_line(0, CYD_COLOR_WHITE, "DAC STOPPED: GPIO44, 43 AND 6 ARE INPUTS");
            info_line(1, CYD_COLOR_GRAY, "BCK  -> GPIO44  UART2 header RXD");
            info_line(2, CYD_COLOR_GRAY, "LRCK -> GPIO43  UART2 header TXD");
            info_line(3, CYD_COLOR_GRAY, "DIN  -> GPIO6   Sensor AD   SCK -> GND");
        }

        for (int pad = 0; pad < PAD_COUNT; ++pad) {
            uint32_t hits = pad_hits[pad];
            if (hits != seen_hits[pad]) {
                seen_hits[pad] = hits;
                lit_until[pad] = now + FLASH_US;
                draw_pad(pad, true);
            } else if (lit_until[pad] != 0 && now >= lit_until[pad]) {
                lit_until[pad] = 0;
                draw_pad(pad, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    QueueHandle_t taps = NULL;
    ESP_ERROR_CHECK(cyd_touch_start(&taps));
    /* Fast mode: a tap on touch-down, every few milliseconds, one per
       finger; the pads are played from the touch task. */
    cyd_touch_set_fast(true);
    cyd_touch_set_tap_callback(on_tap, NULL);

    text_row(TITLE_ROW, CYD_COLOR_WHITE, COLOR_TITLE, "PCM5102A DAC TEST  v" APP_VERSION);
    for (int pad = 0; pad < PAD_COUNT; ++pad) {
        draw_pad(pad, false);
    }
    draw_buttons();
    cyd_draw_small_text(10, HINT_Y, "SW1: UART2 to play, UART1 to flash. STOP first.",
                        CYD_COLOR_GRAY, CYD_COLOR_BLACK);

    /* Above the touch and UI tasks, on core 1, away from this UI loop. */
    xTaskCreatePinnedToCore(audio_task, "pcm5102a", 6144, NULL, 12, NULL, 1);
    ui_loop(taps);
}
