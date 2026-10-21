#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cyd_display.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* CH1 is GPIO35 on the CYD's P3 connector: ADC1 channel 7, input-only, no
   internal pull resistors. ADC1 keeps working whether or not Wi-Fi runs. */
#define CH1_GPIO GPIO_NUM_35
#define CH1_UNIT ADC_UNIT_1
#define CH1_CHANNEL ADC_CHANNEL_7
/* 12 dB attenuation: about 0.15-2.45 V within spec; up to about 3.1 V
   readable, but non-linear above 2.45 V. */
#define CH1_ATTEN ADC_ATTEN_DB_12
#define CH1_BITWIDTH ADC_BITWIDTH_12
#define RAW_MAX 4095
#define RANGE_TOP_MV 2450
/* Used only when the chip has no factory calibration in eFuse. */
#define UNCALIBRATED_FULL_SCALE_MV 3100
#define DEFAULT_VREF_MV 1100

#define SAMPLE_PERIOD_US (3LL * 1000 * 1000)
/* Raw readings averaged into one sample, to tame the ESP32 ADC's noise. */
#define READINGS_PER_SAMPLE 16

#define COLOR_GREEN 0x07E0
#define BUTTON_ROW (CYD_TEXT_ROWS - 1)
#define BUTTON_FIRST_COLUMN 4
#define BUTTON_COLUMNS 9

static const char *TAG = "datalogger";
static QueueHandle_t touch_queue;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration; /* NULL when uncalibrated */

typedef struct {
    bool running;
    int64_t started_us;
    int64_t next_sample_us;
    unsigned count;
    int last_mv;
    int last_raw;
    int min_mv;
    int max_mv;
    int64_t sum_mv;
} session_t;

static session_t session;

/* ---- ADC ---- */

static void adc_init(void)
{
    /* Plain input first: no pulls, nothing driven onto the pin. */
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << CH1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input));

    adc_oneshot_unit_init_cfg_t unit = {.unit_id = CH1_UNIT};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_oneshot_chan_cfg_t channel = {.atten = CH1_ATTEN, .bitwidth = CH1_BITWIDTH};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, CH1_CHANNEL, &channel));

    /* The ESP32 supports line fitting, using the Two Point or Vref values
       burned into eFuse at the factory. */
    adc_cali_line_fitting_efuse_val_t efuse;
    adc_cali_scheme_line_fitting_check_efuse(&efuse);
    adc_cali_line_fitting_config_t cali = {
        .unit_id = CH1_UNIT,
        .atten = CH1_ATTEN,
        .bitwidth = CH1_BITWIDTH,
        .default_vref = DEFAULT_VREF_MV,
    };
    if (efuse == ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF ||
        adc_cali_create_scheme_line_fitting(&cali, &calibration) != ESP_OK) {
        calibration = NULL;
        ESP_LOGW(TAG, "No factory ADC calibration; voltages are estimates");
    } else {
        ESP_LOGI(TAG, "ADC calibration: line fitting from eFuse %s",
                 efuse == ADC_CALI_LINE_FITTING_EFUSE_VAL_EFUSE_TP ? "Two Point" : "Vref");
    }
}

/* One sample: the mean of READINGS_PER_SAMPLE raw readings, in millivolts. */
static bool read_ch1(int *raw, int *millivolts)
{
    int sum = 0;
    for (int i = 0; i < READINGS_PER_SAMPLE; ++i) {
        int reading;
        if (adc_oneshot_read(adc, CH1_CHANNEL, &reading) != ESP_OK) {
            return false;
        }
        sum += reading;
    }
    *raw = (sum + READINGS_PER_SAMPLE / 2) / READINGS_PER_SAMPLE;
    if (calibration != NULL) {
        return adc_cali_raw_to_voltage(calibration, *raw, millivolts) == ESP_OK;
    }
    *millivolts = *raw * UNCALIBRATED_FULL_SCALE_MV / RAW_MAX;
    return true;
}

/* ---- Screen ---- */

static void draw_button(void)
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = session.running ? CYD_COLOR_BLACK : CYD_COLOR_WHITE;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int i = 0; i < BUTTON_COLUMNS; ++i) {
        background[BUTTON_FIRST_COLUMN + i] = session.running ? COLOR_GREEN : CYD_COLOR_BUTTON;
    }
    memcpy(text + BUTTON_FIRST_COLUMN + (BUTTON_COLUMNS - 3) / 2, "CH1", 3);
    ESP_ERROR_CHECK(cyd_draw_cells(BUTTON_ROW, text, foreground, background));
}

static void row(int number, uint16_t color, const char *text)
{
    ESP_ERROR_CHECK(cyd_text_row(number, color, CYD_COLOR_BLACK, text));
}

static void format_volts(char *text, size_t size, const char *label, int millivolts)
{
    snprintf(text, size, "%s%d.%03d V", label, millivolts / 1000, millivolts % 1000);
}

static void draw_screen(void)
{
    char line[32];
    row(0, CYD_COLOR_YELLOW, "DATALOGGER");
    row(1, calibration != NULL ? CYD_COLOR_GRAY : CYD_COLOR_RED,
        calibration != NULL ? "CH1 GPIO35 ADC1" : "CH1 GPIO35 UNCAL");
    row(2, session.running ? COLOR_GREEN : CYD_COLOR_GRAY,
        session.running ? "SAMPLING EVERY 3S" : session.count > 0 ? "STOPPED" : "TAP CH1 TO START");
    if (session.count == 0) {
        for (int i = 3; i < BUTTON_ROW; ++i) {
            row(i, CYD_COLOR_WHITE, i == 3 ? "  -.--- V" : "");
        }
    } else {
        int64_t seconds = (esp_timer_get_time() - session.started_us) / 1000000;
        format_volts(line, sizeof(line), "  ", session.last_mv);
        /* Above the 12 dB range the reading is non-linear: flag it. */
        row(3, session.last_mv > RANGE_TOP_MV ? CYD_COLOR_YELLOW : CYD_COLOR_WHITE, line);
        snprintf(line, sizeof(line), "RAW %d/%d", session.last_raw, RAW_MAX);
        row(4, CYD_COLOR_GRAY, line);
        snprintf(line, sizeof(line), "N %u  T %lld:%02lld", session.count, seconds / 60,
                 seconds % 60);
        row(5, CYD_COLOR_GRAY, line);
        format_volts(line, sizeof(line), "MIN ", session.min_mv);
        row(6, CYD_COLOR_CYAN, line);
        format_volts(line, sizeof(line), "MAX ", session.max_mv);
        row(7, CYD_COLOR_CYAN, line);
        format_volts(line, sizeof(line), "AVG ", (int)(session.sum_mv / session.count));
        row(8, CYD_COLOR_CYAN, line);
    }
    draw_button();
}

/* ---- Session ---- */

static void start_session(void)
{
    memset(&session, 0, sizeof(session));
    session.running = true;
    session.started_us = esp_timer_get_time();
    session.next_sample_us = session.started_us;
    /* CSV on the serial log, ready to paste into a spreadsheet. */
    printf("# CH1 GPIO35 session, every 3 s, %s\n", calibration != NULL ? "calibrated" : "uncalibrated");
    printf("t_s,raw,mV\n");
}

static void take_sample(void)
{
    int raw;
    int millivolts;
    if (!read_ch1(&raw, &millivolts)) {
        ESP_LOGE(TAG, "ADC read failed");
        return;
    }
    if (session.count == 0 || millivolts < session.min_mv) {
        session.min_mv = millivolts;
    }
    if (session.count == 0 || millivolts > session.max_mv) {
        session.max_mv = millivolts;
    }
    session.last_raw = raw;
    session.last_mv = millivolts;
    session.sum_mv += millivolts;
    ++session.count;
    printf("%.1f,%d,%d\n", (esp_timer_get_time() - session.started_us) / 1e6, raw, millivolts);
}

static bool button_tapped(const cyd_touch_point_t *point)
{
    int column = point->x / CYD_FONT_WIDTH;
    return point->y / CYD_FONT_HEIGHT == BUTTON_ROW && column >= BUTTON_FIRST_COLUMN &&
           column < BUTTON_FIRST_COLUMN + BUTTON_COLUMNS;
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    adc_init();
    draw_screen();
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (session.running) {
            int64_t until_ms = (session.next_sample_us - esp_timer_get_time()) / 1000;
            wait = until_ms > 0 ? pdMS_TO_TICKS(until_ms) : 0;
        }
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            /* CH1 starts a session; tapping it again stops it. */
            if (button_tapped(&point)) {
                if (session.running) {
                    session.running = false;
                    printf("# stopped after %u samples\n", session.count);
                } else {
                    start_session();
                }
                draw_screen();
            }
            continue;
        }
        if (session.running && esp_timer_get_time() >= session.next_sample_us) {
            take_sample();
            session.next_sample_us += SAMPLE_PERIOD_US;
            draw_screen();
        }
    }
}
