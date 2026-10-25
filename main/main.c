#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cota_track.h"
#include "cyd_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/* F1 Race: one lap of the Circuit of the Americas, about 30 seconds. The
   first 5 seconds, from the grid up the main straight, are seen over the
   horizon in the manner of OutRun: the road is drawn row by row in
   perspective, each row's depth fixed by its height above the bottom of the
   screen, and bends toward the horizon on curves. Then the view changes to
   a top-down map of the whole circuit for the rest of the lap. Hold the left
   or right half of the screen to steer; the car accelerates by itself and
   slows for corners.

   There is no frame buffer. Over the horizon, the screen is composed in
   20-row strips, each sent with cyd_draw_bitmap; the road strips are redrawn
   every frame, the sky only when the background scrolls or a message
   changes. On the map, only small boxes around the cars are redrawn, from
   1-bit masks of the road and its edges.

   An unlabelled tap target in the top-right corner toggles Hamilton's
   auto-win. With it on (the default), Hamilton, Kimi and Verstappen always
   finish first, second and third; with it off they race at their own pace
   and the result depends on the player's driving. A 2x2 dot in the corner
   shows when it is off. */

static const char *TAG = "f1_race";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define SCREEN_W CYD_LCD_WIDTH
#define SCREEN_H CYD_LCD_HEIGHT
#define STRIP 20                /* rows per cyd_draw_bitmap call */
#define HORIZON 100             /* the first road row */
#define CENTER_X (SCREEN_W / 2)

#define FRAME_MS 40 /* 25 frames per second at most */

/* The world: x across the road (the road is -1..1), s along it. The camera
   sits CAMERA_HEIGHT above the road; a road row dy pixels below the
   horizon (dy = 1 for the first) shows the road at depth DRAW_DISTANCE / dy. */
#define CAMERA_HEIGHT 0.7f
#define FOCAL 160.0f
#define DRAW_DISTANCE (CAMERA_HEIGHT * FOCAL)
#define ROAD_HALF 1.0f
#define RUMBLE 0.14f
#define LANE_LINE 0.03f
#define SEGMENT 4.0f /* stripes alternate every segment */
#define OFFSET_STEP 0.5f
#define OFFSET_COUNT 226 /* DRAW_DISTANCE / OFFSET_STEP + 2 */
#define FOG_START 8.0f

/* The player's car is drawn this far ahead of the camera: 80 pixels wide,
   twice the sprite's size. */
#define PLAYER_Z 0.9f
#define CAR_WIDTH 0.45f
#define CAR_LENGTH 1.0f

/* Speeds in road units per second. TOP_SPEED, ACCELERATION, BRAKING,
   CENTRIFUGAL and DRIFT_MAX are also in tools/cota_track.py, which sizes the
   track from them; change them together and regenerate cota_track.h. */
#define TOP_SPEED 55.0f
#define OFFROAD_SPEED 15.0f
#define COAST_SPEED 20.0f
#define ACCELERATION 7.0f
#define BRAKING 30.0f
#define OFFROAD_BRAKING 45.0f
/* Off the track once a rear wheel is past the road's edge. */
#define OFFROAD_EDGE (ROAD_HALF - CAR_WIDTH * 0.3f)
#define STEER_RATE 1.8f /* across the road per second at top speed */
/* Corners push a car outward by curvature x speed squared x GRIP. Every car
   slows for a corner to the speed where that push is DRIFT_MAX, a little
   less than the player can steer against. */
#define CENTRIFUGAL 0.34f /* the push at top speed, per unit of curvature */
#define GRIP (CENTRIFUGAL / TOP_SPEED)
#define DRIFT_MAX 1.0f
#define PARALLAX 25.0f /* background pixels per unit of curvature and speed */
#define GRASS_EDGE 2.2f
#define RIVAL_STEER 0.9f   /* rivals' sideways speed, half the player's */
#define PASS_REACTION 0.7f /* seconds a rival keeps to the side it picked */
#define MIRROR_RANGE 15.0f /* cars this close behind get a mark */
#define KMH_PER_SPEED 6

/* The track is COTA, from cota_track.h. s counts road units from the grid,
   COTA_GRID_BACK behind the start line, and the race ends one lap later at
   the start line. */
#define RACE_LENGTH ((float)(COTA_GRID_BACK + COTA_LENGTH))
#define HORIZON_SECONDS 5.0f /* then the top-down map */
#define FINISH_SECONDS 2.5f

/* The map: the whole circuit under a 20-row bar, the road drawn ROAD_PX
   either side of the centerline. A car's place across the road maps to the
   same scale, so the edge of the drawn road is the edge of the track. */
#define MAP_TOP STRIP
#define ROAD_PX 3
#define MAP_BOX 7 /* half the side of the box redrawn around a car */
/* The map's touch zones: gas and brake left of CENTER_X, split at
   PEDAL_SPLIT_Y; steering right of it, split at STEER_SPLIT_X. */
#define PEDAL_SPLIT_Y 130
#define STEER_SPLIT_X 240
#define COAST_DRAG 3.0f /* slowing with neither pedal, units/s^2 */
#define COLOR_MAP_GUIDE RGB565(0, 95, 0)
#define COLOR_MAP_GRASS RGB565(0, 130, 0)
#define COLOR_MAP_ROAD RGB565(100, 100, 100)
#define COLOR_MAP_EDGE RGB565(235, 235, 235)

/* Hamilton's auto-win: rivals close up on the player from this far into the
   race, to these leads at the finish. */
#define RIG_FROM 0.45f
#define PACE_JITTER 0.006f
#define RIG_RAMP 0.4f
#define RIG_CLAMP 0.9f
#define RIG_SPEEDUP 1.35f /* at most this times the safe speed */
/* From here on, rigged rivals squeeze past a blocking player. */
#define RIG_SQUEEZE 0.75f

/* The secret corner. */
#define SECRET_SIZE 40
#define INDICATOR_X 316
#define INDICATOR_Y 1
#define INDICATOR_SIZE 2

/* Touch readings drop out now and then while a finger is held. */
#define RELEASE_FRAMES 3

#define COLOR_SKY_TOP RGB565(30, 80, 200)
#define COLOR_HAZE RGB565(170, 210, 250)
#define COLOR_MOUNTAIN RGB565(90, 105, 160)
#define COLOR_SNOW RGB565(230, 235, 245)
#define COLOR_HILLS RGB565(40, 120, 50)
#define COLOR_GRASS_LIGHT RGB565(16, 200, 16)
#define COLOR_GRASS_DARK RGB565(0, 154, 0)
#define COLOR_RUMBLE_LIGHT RGB565(255, 255, 255)
#define COLOR_RUMBLE_DARK RGB565(210, 0, 0)
#define COLOR_ROAD_LIGHT RGB565(112, 112, 112)
#define COLOR_ROAD_DARK RGB565(96, 96, 96)
#define COLOR_LANE RGB565(240, 240, 240)
#define COLOR_SHADOW RGB565(0, 0, 0)
#define COLOR_INDICATOR RGB565(60, 60, 60)
#define COLOR_GREEN RGB565(40, 230, 60)
#define TRANSPARENT 0xF81F

/* 3x5 glyphs, one byte per row, bit 2 on the left. */
static const uint8_t FONT[128][5] = {
    ['0'] = {7, 5, 5, 5, 7}, ['1'] = {2, 6, 2, 2, 7}, ['2'] = {7, 1, 7, 4, 7},
    ['3'] = {7, 1, 7, 1, 7}, ['4'] = {5, 5, 7, 1, 1}, ['5'] = {7, 4, 7, 1, 7},
    ['6'] = {7, 4, 7, 5, 7}, ['7'] = {7, 1, 1, 2, 2}, ['8'] = {7, 5, 7, 5, 7},
    ['9'] = {7, 5, 7, 1, 7}, ['A'] = {2, 5, 7, 5, 5}, ['B'] = {6, 5, 6, 5, 6},
    ['C'] = {3, 4, 4, 4, 3}, ['D'] = {6, 5, 5, 5, 6}, ['E'] = {7, 4, 6, 4, 7},
    ['F'] = {7, 4, 6, 4, 4}, ['G'] = {3, 4, 5, 5, 3}, ['H'] = {5, 5, 7, 5, 5},
    ['I'] = {7, 2, 2, 2, 7}, ['J'] = {1, 1, 1, 5, 2}, ['K'] = {5, 5, 6, 5, 5},
    ['L'] = {4, 4, 4, 4, 7}, ['M'] = {5, 7, 7, 5, 5}, ['N'] = {6, 5, 5, 5, 5},
    ['O'] = {2, 5, 5, 5, 2}, ['P'] = {6, 5, 6, 4, 4}, ['Q'] = {2, 5, 5, 6, 3},
    ['R'] = {6, 5, 6, 5, 5}, ['S'] = {3, 4, 2, 1, 6}, ['T'] = {7, 2, 2, 2, 2},
    ['U'] = {5, 5, 5, 5, 7}, ['V'] = {5, 5, 5, 5, 2}, ['W'] = {5, 5, 7, 7, 5},
    ['X'] = {5, 5, 2, 5, 5}, ['Y'] = {5, 5, 2, 2, 2}, ['Z'] = {7, 1, 2, 4, 7},
    ['/'] = {1, 1, 2, 4, 4}, ['!'] = {2, 2, 2, 0, 2}, ['.'] = {0, 0, 0, 0, 2},
    ['+'] = {0, 2, 7, 2, 0}, ['-'] = {0, 0, 7, 0, 0}, [':'] = {0, 2, 0, 2, 0},
    ['<'] = {1, 2, 4, 2, 1}, ['>'] = {4, 2, 1, 2, 4},
};

/* An F1 car from behind: 'c' body, 'd' trim, 'h' helmet, 'k' tires and
   diffuser, 'g' gray, 'r' rain light, '.' transparent. The car's number
   goes on the engine cover. */
#define SPRITE_W 40
#define SPRITE_H 20
static const char CAR_SPRITE[SPRITE_H][SPRITE_W + 1] = {
    "......dddddddddddddddddddddddddddd......",
    "......dccccccccccccccccccccccccccd......",
    "......dccccccccccccccccccccccccccd......",
    "......dddddddddddddddddddddddddddd......",
    "......d...........hhhh...........d......",
    "......d..........hhhhhh..........d......",
    "......d........cccccccccc........d......",
    ".kkkkkkk.....cccccccccccccc.....kkkkkkk.",
    "kkkkkkkkk...cccccccccccccccc...kkkkkkkkk",
    "kgkkkkkkk.cccccccccccccccccccc.kkkkkkkgk",
    "kgkkkkkkkgccccccccccccccccccccgkkkkkkkgk",
    "kgkkkkkkk.cccccccccccccccccccc.kkkkkkkgk",
    "kgkkkkkkk.cccccccccccccccccccc.kkkkkkkgk",
    "kgkkkkkkkgccccccccccccccccccccgkkkkkkkgk",
    "kgkkkkkkk.dddddddddddddddddddd.kkkkkkkgk",
    "kgkkkkkkk..kkkkkkkkkkkkkkkkkk..kkkkkkkgk",
    "kgkkkkkkk..kkgkkgkkrrkkgkkgkk..kkkkkkkgk",
    "kgkkkkkkk..kkgkkgkkrrkkgkkgkk..kkkkkkkgk",
    "kkkkkkkkk..kkgkkgkkkkkkgkkgkk..kkkkkkkkk",
    ".kkkkkkk........................kkkkkkk.",
};
#define NUMBER_ROW 9

enum { HAMILTON, KIMI, VERSTAPPEN, PLAYER, CAR_COUNT };

typedef struct {
    const char *name;
    int number; /* 0: none */
    uint16_t body, trim, numeral, helmet;
    uint16_t label;  /* name color on the results screen */
    float base_pace; /* share of TOP_SPEED */
    float pace;      /* this race's, varied a little */
    float lead;      /* when rigged, the lead over the player at the finish */
    /* Race state. */
    float s, x, v;
    float lane, lane_timer;
    float pass_timer;
    float finish_time; /* seconds, or negative before the finish */
    uint16_t pixels[SPRITE_H * SPRITE_W];
} car_t;

static car_t cars[CAR_COUNT] = {
    [HAMILTON] = {.name = "HAMILTON", .number = 44, .body = RGB565(220, 0, 0),
                  .trim = RGB565(120, 0, 0), .numeral = RGB565(255, 255, 255),
                  .helmet = RGB565(255, 220, 0), .label = RGB565(255, 60, 60),
                  .base_pace = 0.92f, .lead = 30},
    [KIMI] = {.name = "KIMI", .number = 12, .body = RGB565(185, 190, 195),
              .trim = RGB565(0, 170, 160), .numeral = RGB565(20, 20, 20),
              .helmet = RGB565(40, 120, 255), .label = RGB565(0, 220, 200),
              .base_pace = 0.91f, .lead = 18},
    [VERSTAPPEN] = {.name = "VERSTAPPEN", .number = 3, .body = RGB565(25, 35, 110),
                    .trim = RGB565(220, 20, 50), .numeral = RGB565(255, 200, 0),
                    .helmet = RGB565(255, 120, 0), .label = RGB565(110, 140, 255),
                    .base_pace = 0.90f, .lead = 8},
    [PLAYER] = {.name = "YOU", .body = RGB565(255, 210, 0), .trim = RGB565(30, 30, 30),
                .helmet = RGB565(255, 255, 255), .label = CYD_COLOR_YELLOW},
};

static const float track_length = RACE_LENGTH;
/* The fastest each road unit can be taken, braking in time for the next
   corner. */
static float safe_speed[COTA_LENGTH];

/* 1-bit masks of the map: the road, and the road with its edge lines. */
static uint8_t road_mask[SCREEN_H][SCREEN_W / 8];
static uint8_t edge_mask[SCREEN_H][SCREEN_W / 8];

enum { VIEW_HORIZON, VIEW_MAP };
static int view;

enum { PEDAL_NONE, PEDAL_GAS, PEDAL_BRAKE };
static int shown_pedal; /* the pedal held, for the bar */
static int boxes[CAR_COUNT][4]; /* each car's last box on the map: x0, y0, x1, y1 */

static bool rigged = true; /* Hamilton's auto-win */
static float race_time;
static int frame_count;

/* Per frame: the road's sideways offset at each depth from the camera. */
static float offsets[OFFSET_COUNT];
static float camera_s;
static float bg_scroll;

static const char *message;
static uint16_t message_color;
static const char *drawn_message;
static int drawn_far = -1, drawn_near = -1;
static bool sky_dirty = true;

static uint16_t strip[STRIP * SCREEN_W];
static uint16_t sky_colors[HORIZON];
static uint8_t far_heights[512];
static uint8_t near_heights[512];

typedef struct {
    int x, y, w, h; /* screen rectangle, top-left */
    float z;
    const uint16_t *pixels;
} sprite_t;

static sprite_t sprites[CAR_COUNT];
static int sprite_count;

static float random_range(float low, float high)
{
    return low + (high - low) * (float)(esp_random() >> 8) / (float)(1 << 24);
}

static uint16_t blend(uint16_t a, uint16_t b, int t)
{
    int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
    int g = (((a >> 5) & 0x3F) * (256 - t) + ((b >> 5) & 0x3F) * t) >> 8;
    int bl = ((a & 0x1F) * (256 - t) + (b & 0x1F) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* The COTA table entry for race position s. */
static int track_index(float s)
{
    int i = ((int)floorf(s) - COTA_GRID_BACK) % COTA_LENGTH;
    return i < 0 ? i + COTA_LENGTH : i;
}

static float curvature_at(float s)
{
    return COTA_CURVATURE[track_index(s)] / COTA_CURVATURE_SCALE;
}

/* Distance past the start line, within the lap. */
static float lap_position(float s)
{
    float p = fmodf(s - COTA_GRID_BACK, COTA_LENGTH);
    return p < 0 ? p + COTA_LENGTH : p;
}

/* ---- Setup ---- */

static void build_sprites(void)
{
    for (int i = 0; i < CAR_COUNT; ++i) {
        car_t *car = &cars[i];
        for (int y = 0; y < SPRITE_H; ++y) {
            for (int x = 0; x < SPRITE_W; ++x) {
                uint16_t color = TRANSPARENT;
                switch (CAR_SPRITE[y][x]) {
                case 'c': color = car->body; break;
                case 'd': color = car->trim; break;
                case 'h': color = car->helmet; break;
                case 'k': color = RGB565(25, 25, 25); break;
                case 'g': color = RGB565(110, 110, 110); break;
                case 'r': color = RGB565(255, 40, 40); break;
                default: break;
                }
                car->pixels[y * SPRITE_W + x] = color;
            }
        }
        if (car->number == 0) {
            continue;
        }
        /* One digit centered, or two either side of the middle. */
        char digits[4];
        snprintf(digits, sizeof(digits), "%d", car->number % 100);
        int count = (int)strlen(digits);
        for (int d = 0; d < count; ++d) {
            int left = count == 1 ? 18 : 16 + d * 5;
            const uint8_t *glyph = FONT[(int)digits[d]];
            for (int gy = 0; gy < 5; ++gy) {
                for (int gx = 0; gx < 3; ++gx) {
                    if (glyph[gy] & (4 >> gx)) {
                        car->pixels[(NUMBER_ROW + gy) * SPRITE_W + left + gx] = car->numeral;
                    }
                }
            }
        }
    }
}

static void build_scenery(void)
{
    for (int y = 0; y < HORIZON; ++y) {
        sky_colors[y] = blend(COLOR_SKY_TOP, COLOR_HAZE, y * 256 / HORIZON);
    }
    const float tau = 6.2831853f / 512;
    for (int x = 0; x < 512; ++x) {
        far_heights[x] = (uint8_t)(22 + 10 * sinf(tau * 2 * x) + 7 * sinf(tau * 5 * x + 1.3f) +
                                   3 * sinf(tau * 11 * x + 0.4f));
        near_heights[x] = (uint8_t)(8 + 4 * sinf(tau * 3 * x + 2) + 3 * sinf(tau * 7 * x));
    }
}

static void stamp_disc(uint8_t mask[SCREEN_H][SCREEN_W / 8], int cx, int cy, int radius)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int x = cx + dx;
            int y = cy + dy;
            if (dx * dx + dy * dy <= radius * radius && x >= 0 && x < SCREEN_W && y >= 0 &&
                y < SCREEN_H) {
                mask[y][x >> 3] |= (uint8_t)(1 << (x & 7));
            }
        }
    }
}

static bool mask_bit(uint8_t mask[SCREEN_H][SCREEN_W / 8], int x, int y)
{
    return mask[y][x >> 3] & (1 << (x & 7));
}

/* Safe corner speeds (as in tools/cota_track.py) and the map masks. */
static void build_track(void)
{
    for (int i = 0; i < COTA_LENGTH; ++i) {
        float c = fabsf(COTA_CURVATURE[i] / COTA_CURVATURE_SCALE);
        safe_speed[i] = c > 0 ? fminf(TOP_SPEED, sqrtf(DRIFT_MAX / (GRIP * c))) : TOP_SPEED;
    }
    for (int pass = 0; pass < 2; ++pass) { /* twice, so braking zones wrap */
        for (int i = COTA_LENGTH - 1; i >= 0; --i) {
            float next = safe_speed[(i + 1) % COTA_LENGTH];
            safe_speed[i] = fminf(safe_speed[i], sqrtf(next * next + 2 * BRAKING));
        }
    }
    for (int i = 0; i < COTA_LENGTH; ++i) {
        int x = (int)lroundf(COTA_X[i] / COTA_POSITION_SCALE);
        int y = (int)lroundf(COTA_Y[i] / COTA_POSITION_SCALE);
        stamp_disc(road_mask, x, y, ROAD_PX);
        stamp_disc(edge_mask, x, y, ROAD_PX + 1);
    }
    ESP_LOGI(TAG, "COTA: %d units of %.2f m, grid %d units back", COTA_LENGTH,
             COTA_METERS_PER_UNIT, COTA_GRID_BACK);
}

/* The fastest a car can be going at race position s. */
static float safe_speed_at(float s)
{
    return safe_speed[track_index(s)];
}

static void reset_cars(void)
{
    /* A 2x2 grid: Hamilton and Kimi on the front row, Verstappen beside the
       player on the second, a nose ahead so the player starts fourth. */
    static const float grid_s[CAR_COUNT] = {2.2f, 2.2f, 0.05f, 0};
    static const float grid_x[CAR_COUNT] = {-0.3f, 0.3f, -0.3f, 0.3f};
    for (int i = 0; i < CAR_COUNT; ++i) {
        car_t *car = &cars[i];
        car->s = grid_s[i];
        car->x = car->lane = grid_x[i];
        car->v = 0;
        car->lane_timer = random_range(3, 6);
        car->pass_timer = 0;
        car->finish_time = -1;
        car->pace = car->base_pace + random_range(-PACE_JITTER, PACE_JITTER);
    }
    bg_scroll = 0;
    race_time = 0;
    sky_dirty = true;
    view = VIEW_HORIZON;
}

static void load_rigged(void)
{
    nvs_handle_t nvs;
    uint8_t value = 1;
    if (nvs_flash_init() == ESP_OK && nvs_open("f1_race", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "rigged", &value);
        nvs_close(nvs);
    }
    rigged = value != 0;
}

static void toggle_rigged(void)
{
    nvs_handle_t nvs;
    rigged = !rigged;
    ESP_LOGI(TAG, "Hamilton's auto-win %s", rigged ? "on" : "off");
    if (nvs_open("f1_race", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "rigged", rigged ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool in_secret_corner(const cyd_touch_point_t *point)
{
    return point->x >= SCREEN_W - SECRET_SIZE && point->y < SECRET_SIZE;
}

/* ---- Drawing into the strip ---- */

/* Fills [x0, x1) of one strip row, rounded and clipped to the screen. */
static void fill_span(uint16_t *row, float x0, float x1, uint16_t color)
{
    int from = (int)floorf(x0 + 0.5f);
    int to = (int)floorf(x1 + 0.5f);
    from = from < 0 ? 0 : from;
    to = to > SCREEN_W ? SCREEN_W : to;
    for (int x = from; x < to; ++x) {
        row[x] = color;
    }
}

/* A rectangle in screen coordinates, clipped to the strip starting at y0. */
static void strip_rect(int y0, int x, int y, int w, int h, uint16_t color)
{
    int top = y > y0 ? y : y0;
    int bottom = y + h < y0 + STRIP ? y + h : y0 + STRIP;
    int left = x > 0 ? x : 0;
    int right = x + w < SCREEN_W ? x + w : SCREEN_W;
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            strip[(row - y0) * SCREEN_W + column] = color;
        }
    }
}

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 4 * scale - scale;
}

static void strip_text(int y0, int x, int y, int scale, uint16_t color, const char *text)
{
    for (; *text != '\0'; ++text, x += 4 * scale) {
        const uint8_t *glyph = FONT[*text & 0x7F];
        for (int gy = 0; gy < 5; ++gy) {
            for (int gx = 0; gx < 3; ++gx) {
                if (glyph[gy] & (4 >> gx)) {
                    strip_rect(y0, x + gx * scale, y + gy * scale, scale, scale, color);
                }
            }
        }
    }
}

/* Text with a drop shadow, for reading against the sky. */
static void strip_label(int y0, int x, int y, int scale, uint16_t color, const char *text)
{
    int shadow = scale > 2 ? scale / 2 : 1;
    strip_text(y0, x + shadow, y + shadow, scale, COLOR_SHADOW, text);
    strip_text(y0, x, y, scale, color, text);
}

static void strip_sprites(int y0)
{
    for (int i = 0; i < sprite_count; ++i) {
        const sprite_t *sprite = &sprites[i];
        int top = sprite->y > y0 ? sprite->y : y0;
        int bottom = sprite->y + sprite->h < y0 + STRIP ? sprite->y + sprite->h : y0 + STRIP;
        int left = sprite->x > 0 ? sprite->x : 0;
        int right = sprite->x + sprite->w < SCREEN_W ? sprite->x + sprite->w : SCREEN_W;
        for (int y = top; y < bottom; ++y) {
            const uint16_t *source =
                sprite->pixels + (y - sprite->y) * SPRITE_H / sprite->h * SPRITE_W;
            uint16_t *row = strip + (y - y0) * SCREEN_W;
            for (int x = left; x < right; ++x) {
                uint16_t color = source[(x - sprite->x) * SPRITE_W / sprite->w];
                if (color != TRANSPARENT) {
                    row[x] = color;
                }
            }
        }
    }
}

static int car_place(const car_t *car)
{
    int place = 1;
    for (int i = 0; i < CAR_COUNT; ++i) {
        const car_t *other = &cars[i];
        if (other == car) {
            continue;
        }
        bool ahead = car->finish_time >= 0
                         ? other->finish_time >= 0 && other->finish_time < car->finish_time
                         : other->finish_time >= 0 || other->s > car->s;
        place += ahead ? 1 : 0;
    }
    return place;
}

/* The bar across the top strip: place, speed and everyone's progress (or,
   on the map, a message in its place), and the secret's indicator. */
static void strip_hud(bool on_map)
{
    char text[16];
    snprintf(text, sizeof(text), "P%d/%d", car_place(&cars[PLAYER]), CAR_COUNT);
    strip_label(0, 6, 5, 2, CYD_COLOR_WHITE, text);
    int kmh = (int)(cars[PLAYER].v * KMH_PER_SPEED + 0.5f);
    snprintf(text, sizeof(text), "%d KMH", kmh % 1000);
    strip_label(0, SCREEN_W - 10 - text_width(text, 2), 5, 2, CYD_COLOR_WHITE, text);
    if (on_map && shown_pedal != PEDAL_NONE) {
        bool gas = shown_pedal == PEDAL_GAS;
        strip_label(0, 44, 5, 2, gas ? COLOR_GREEN : CYD_COLOR_RED, gas ? "GAS" : "BRAKE");
    }
    if (on_map && message != NULL) {
        strip_label(0, (SCREEN_W - text_width(message, 2)) / 2, 5, 2, message_color, message);
    } else {
        /* A mark for each car; the player's on top. */
        const int bar_x = 100, bar_w = 120;
        strip_rect(0, bar_x + 1, 10, bar_w, 2, COLOR_SHADOW);
        strip_rect(0, bar_x, 9, bar_w, 2, RGB565(230, 230, 230));
        for (int i = 0; i < CAR_COUNT; ++i) {
            float progress = fminf(1, fmaxf(0, cars[i].s / track_length));
            int x = bar_x + (int)(progress * (bar_w - 3));
            strip_rect(0, x, 6, 3, 8, i == PLAYER ? CYD_COLOR_YELLOW : cars[i].label);
        }
    }
    if (!rigged) {
        strip_rect(0, INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE, COLOR_INDICATOR);
    }
}

static void draw_sky_strip(int y0)
{
    int far = (int)floorf(bg_scroll * 0.5f);
    int near = (int)floorf(bg_scroll);
    for (int r = 0; r < STRIP; ++r) {
        int y = y0 + r;
        uint16_t *row = strip + r * SCREEN_W;
        uint16_t sky = sky_colors[y];
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t color = sky;
            int peak = HORIZON - far_heights[(x + far) & 511];
            if (y >= HORIZON - near_heights[(x + near) & 511]) {
                color = COLOR_HILLS;
            } else if (y >= peak) {
                color = y < peak + 3 && peak < HORIZON - 30 ? COLOR_SNOW : COLOR_MOUNTAIN;
            }
            row[x] = color;
        }
    }

    if (message != NULL) {
        strip_label(y0, (SCREEN_W - text_width(message, 6)) / 2, 30, 6, message_color, message);
    }
    if (y0 == 0) {
        strip_hud(false);
    }
    cyd_draw_bitmap(0, y0, SCREEN_W, STRIP, strip);
}

/* Cars just behind, which the camera can't see: a mark along the bottom
   edge under each one, wider when it is close. */
static void strip_mirror(int y0)
{
    const car_t *player = &cars[PLAYER];
    for (int i = 0; i < PLAYER; ++i) {
        const car_t *car = &cars[i];
        float behind = player->s - car->s;
        if (behind < PLAYER_Z - 0.35f || behind > MIRROR_RANGE) {
            continue; /* on screen, or far back */
        }
        int w = behind < 4 ? 24 : 14;
        int x = CENTER_X + (int)((car->x - player->x) * FOCAL / PLAYER_Z) - w / 2;
        x = x < 2 ? 2 : x > SCREEN_W - 2 - w ? SCREEN_W - 2 - w : x;
        strip_rect(y0, x - 1, SCREEN_H - 7, w + 2, 6, COLOR_SHADOW);
        strip_rect(y0, x, SCREEN_H - 6, w, 4, car->label);
    }
}

static void draw_road_strip(int y0)
{
    float player_x = cars[PLAYER].x;
    for (int r = 0; r < STRIP; ++r) {
        uint16_t *row = strip + r * SCREEN_W;
        int dy = y0 + r - HORIZON + 1;
        float z = DRAW_DISTANCE / dy;
        float scale = FOCAL / z;
        float index = z / OFFSET_STEP;
        int i = (int)index;
        float offset = i + 1 < OFFSET_COUNT
                           ? offsets[i] + (offsets[i + 1] - offsets[i]) * (index - i)
                           : offsets[OFFSET_COUNT - 1];
        float center = CENTER_X + (offset - player_x) * scale;
        float half = ROAD_HALF * scale;
        float rumble = RUMBLE * scale;
        float s = camera_s + z;
        bool light = ((int)floorf(s / SEGMENT)) & 1;
        int fog = z <= FOG_START ? 0
                                 : (int)(230 * (z - FOG_START) / (DRAW_DISTANCE - FOG_START));

        fill_span(row, 0, SCREEN_W,
                  blend(light ? COLOR_GRASS_LIGHT : COLOR_GRASS_DARK, COLOR_HAZE, fog));
        uint16_t rumble_color =
            blend(light ? COLOR_RUMBLE_LIGHT : COLOR_RUMBLE_DARK, COLOR_HAZE, fog);
        fill_span(row, center - half - rumble, center - half, rumble_color);
        fill_span(row, center + half, center + half + rumble, rumble_color);

        bool checkered = lap_position(s) < 3; /* the start line */
        if (checkered) {
            int left = (int)floorf(center - half + 0.5f);
            int right = (int)floorf(center + half + 0.5f);
            left = left < 0 ? 0 : left;
            right = right > SCREEN_W ? SCREEN_W : right;
            int band = (int)floorf(s * 1.5f);
            for (int x = left; x < right; ++x) {
                int cell = (int)floorf(((float)x - center) / half * 5 + 5) + band;
                row[x] = blend(cell & 1 ? CYD_COLOR_WHITE : CYD_COLOR_BLACK, COLOR_HAZE, fog);
            }
            continue;
        }
        fill_span(row, center - half, center + half,
                  blend(light ? COLOR_ROAD_LIGHT : COLOR_ROAD_DARK, COLOR_HAZE, fog));
        float line = LANE_LINE * scale;
        if (light && line >= 0.5f) {
            uint16_t lane_color = blend(COLOR_LANE, COLOR_HAZE, fog);
            line = fmaxf(line, 1);
            for (int lane = -1; lane <= 1; lane += 2) {
                float x = center + lane * half / 3;
                fill_span(row, x - line / 2, x + line / 2, lane_color);
            }
        }
    }
    strip_sprites(y0);
    if (y0 + STRIP == SCREEN_H) {
        strip_mirror(y0);
    }
    cyd_draw_bitmap(0, y0, SCREEN_W, STRIP, strip);
}

/* The road's sideways offset ahead of the camera, integrated from the
   curvature: the camera always looks along the road where it is. */
static void compute_offsets(void)
{
    float offset = 0;
    float slope = 0;
    offsets[0] = 0;
    for (int i = 1; i < OFFSET_COUNT; ++i) {
        float c = curvature_at(camera_s + (i - 0.5f) * OFFSET_STEP);
        offset += slope * OFFSET_STEP + 0.5f * c * OFFSET_STEP * OFFSET_STEP;
        slope += c * OFFSET_STEP;
        offsets[i] = offset;
    }
}

static void place_sprites(void)
{
    sprite_count = 0;
    for (int i = 0; i < CAR_COUNT; ++i) {
        const car_t *car = &cars[i];
        float z = car->s - camera_s;
        if (z < 0.35f || z > DRAW_DISTANCE) {
            continue;
        }
        float scale = FOCAL / z;
        int w = (int)(CAR_WIDTH * scale + 0.5f);
        if (w < 2) {
            continue;
        }
        float index = z / OFFSET_STEP;
        int k = (int)index;
        float offset = k + 1 < OFFSET_COUNT
                           ? offsets[k] + (offsets[k + 1] - offsets[k]) * (index - k)
                           : offsets[OFFSET_COUNT - 1];
        sprite_t *sprite = &sprites[sprite_count++];
        sprite->w = w;
        sprite->h = w / 2 > 1 ? w / 2 : 1;
        sprite->x = (int)floorf(CENTER_X + (car->x + offset - cars[PLAYER].x) * scale - w / 2.0f +
                                0.5f);
        sprite->y = HORIZON - 1 + (int)(DRAW_DISTANCE / z + 0.5f) - sprite->h + 1;
        if (i == PLAYER && fabsf(car->x) > OFFROAD_EDGE && car->v > 1) {
            sprite->y -= (frame_count & 1) * 2; /* bumping over the grass */
        }
        sprite->z = z;
        sprite->pixels = car->pixels;
    }
    /* Far to near. */
    for (int i = 1; i < sprite_count; ++i) {
        sprite_t sprite = sprites[i];
        int j = i - 1;
        while (j >= 0 && sprites[j].z < sprite.z) {
            sprites[j + 1] = sprites[j];
            --j;
        }
        sprites[j + 1] = sprite;
    }
}

/* ---- The top-down map ---- */

typedef struct {
    float x, y;     /* map pixels */
    float cos, sin; /* heading */
    uint16_t body, outline;
} map_car_t;

static map_car_t map_cars[CAR_COUNT];

/* Each car on the map: the centerline point at its place along the lap,
   moved across the road by its place across it. */
static void place_map_cars(void)
{
    for (int i = 0; i < CAR_COUNT; ++i) {
        const car_t *car = &cars[i];
        int k = track_index(car->s);
        int next = (k + 1) % COTA_LENGTH;
        float f = car->s - floorf(car->s);
        float cx = (COTA_X[k] + (COTA_X[next] - COTA_X[k]) * f) / COTA_POSITION_SCALE;
        float cy = (COTA_Y[k] + (COTA_Y[next] - COTA_Y[k]) * f) / COTA_POSITION_SCALE;
        float heading = COTA_HEADING[k] / COTA_HEADING_SCALE;
        map_car_t *m = &map_cars[i];
        m->cos = cosf(heading);
        m->sin = sinf(heading);
        /* The driver's right is the heading turned a quarter clockwise. */
        m->x = cx - m->sin * car->x * ROAD_PX;
        m->y = cy + m->cos * car->x * ROAD_PX;
        m->body = car->label;
        m->outline = i == PLAYER ? CYD_COLOR_WHITE : CYD_COLOR_BLACK;
    }
}

/* A car seen from above: a 7x4 body pointing along its heading, with a
   narrower nose, outlined. */
static bool map_car_pixel(const map_car_t *m, int x, int y, uint16_t *color)
{
    float dx = x + 0.5f - m->x;
    float dy = y + 0.5f - m->y;
    float along = dx * m->cos + dy * m->sin;
    float across = -dx * m->sin + dy * m->cos;
    if (fabsf(along) > 4.5f || fabsf(across) > 3.0f) {
        return false;
    }
    bool body = fabsf(along) <= 3.5f && fabsf(across) <= 2.0f && !(along > 2 && fabsf(across) > 1);
    *color = body ? m->body : m->outline;
    return true;
}

/* The chequered start line across the road at the first table entry. */
static bool finish_pixel(int x, int y, uint16_t *color)
{
    float dx = x + 0.5f - COTA_X[0] / COTA_POSITION_SCALE;
    float dy = y + 0.5f - COTA_Y[0] / COTA_POSITION_SCALE;
    if (fabsf(dx) > 6 || fabsf(dy) > 6) {
        return false;
    }
    float heading = COTA_HEADING[0] / COTA_HEADING_SCALE;
    float along = dx * cosf(heading) + dy * sinf(heading);
    float across = -dx * sinf(heading) + dy * cosf(heading);
    if (fabsf(along) >= 1.5f || fabsf(across) >= ROAD_PX + 0.5f) {
        return false;
    }
    *color = ((int)floorf(along + 2) + (int)floorf(across + 8)) & 1 ? CYD_COLOR_WHITE
                                                                       : CYD_COLOR_BLACK;
    return true;
}

/* Labels for the touch zones, on grass in each zone (the spots were found
   against the road mask, keeping clear of the secret corner). */
typedef struct {
    const char *text;
    int x, y, scale;
} map_label_t;

static const map_label_t MAP_LABELS[] = {
    {"GAS", 78, 54, 2}, {"BRAKE", 60, 179, 2}, {"<", 194, 90, 4}, {">", 273, 129, 4},
};

static bool label_glyph(const map_label_t *label, int dx, int dy)
{
    int advance = 4 * label->scale;
    if (dx < 0 || dy < 0 || dy >= 5 * label->scale ||
        dx / advance >= (int)strlen(label->text)) {
        return false;
    }
    int gx = dx % advance / label->scale;
    int gy = dy / label->scale;
    return gx < 3 && (FONT[label->text[dx / advance] & 0x7F][gy] & (4 >> gx));
}

static bool label_pixel(int x, int y, uint16_t *color)
{
    for (size_t i = 0; i < sizeof(MAP_LABELS) / sizeof(MAP_LABELS[0]); ++i) {
        const map_label_t *label = &MAP_LABELS[i];
        if (label_glyph(label, x - label->x, y - label->y)) {
            *color = CYD_COLOR_WHITE;
            return true;
        }
        if (label_glyph(label, x - label->x - 1, y - label->y - 1)) {
            *color = COLOR_SHADOW;
            return true;
        }
    }
    return false;
}

static uint16_t map_pixel(int x, int y)
{
    uint16_t color;
    for (int i = CAR_COUNT - 1; i >= 0; --i) { /* the player on top */
        if (map_car_pixel(&map_cars[i], x, y, &color)) {
            return color;
        }
    }
    if (finish_pixel(x, y, &color)) {
        return color;
    }
    if (mask_bit(road_mask, x, y)) {
        return COLOR_MAP_ROAD;
    }
    if (mask_bit(edge_mask, x, y)) {
        return COLOR_MAP_EDGE;
    }
    if (label_pixel(x, y, &color)) {
        return color;
    }
    /* Dotted borders between the touch zones, on the grass only. */
    bool border = x == CENTER_X || (x > CENTER_X && x == STEER_SPLIT_X) ||
                  (x < CENTER_X && y == PEDAL_SPLIT_Y);
    return border && ((x + y) & 2) ? COLOR_MAP_GUIDE : COLOR_MAP_GRASS;
}

static void draw_map_hud(void)
{
    for (int i = 0; i < STRIP * SCREEN_W; ++i) {
        strip[i] = CYD_COLOR_BLACK;
    }
    strip_hud(true);
    cyd_draw_bitmap(0, 0, SCREEN_W, STRIP, strip);
}

/* Redraws a box of the map, clipped to the map's area. */
static void draw_map_box(const int box[4])
{
    int x0 = box[0] > 0 ? box[0] : 0;
    int y0 = box[1] > MAP_TOP ? box[1] : MAP_TOP;
    int x1 = box[2] < SCREEN_W ? box[2] : SCREEN_W;
    int y1 = box[3] < SCREEN_H ? box[3] : SCREEN_H;
    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0 || w * h > STRIP * SCREEN_W) {
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            strip[(y - y0) * w + (x - x0)] = map_pixel(x, y);
        }
    }
    cyd_draw_bitmap(x0, y0, w, h, strip);
}

/* The whole map, when the view changes. */
static void draw_map(void)
{
    place_map_cars();
    draw_map_hud();
    for (int y0 = MAP_TOP; y0 < SCREEN_H; y0 += STRIP) {
        for (int r = 0; r < STRIP; ++r) {
            for (int x = 0; x < SCREEN_W; ++x) {
                strip[r * SCREEN_W + x] = map_pixel(x, y0 + r);
            }
        }
        cyd_draw_bitmap(0, y0, SCREEN_W, STRIP, strip);
    }
    for (int i = 0; i < CAR_COUNT; ++i) {
        boxes[i][0] = boxes[i][2] = -1; /* nothing to erase yet */
    }
}

/* Each frame on the map: erase each car where it was and draw it where it
   is. Both boxes are composed from everything, so overlaps come out right. */
static void render_map(void)
{
    place_map_cars();
    draw_map_hud();
    for (int i = 0; i < CAR_COUNT; ++i) {
        if (boxes[i][2] > boxes[i][0]) {
            draw_map_box(boxes[i]);
        }
        int x = (int)floorf(map_cars[i].x);
        int y = (int)floorf(map_cars[i].y);
        int box[4] = {x - MAP_BOX, y - MAP_BOX, x + MAP_BOX + 1, y + MAP_BOX + 1};
        draw_map_box(box);
        memcpy(boxes[i], box, sizeof(box));
    }
}

static void render(void)
{
    if (view == VIEW_MAP) {
        render_map();
        return;
    }
    camera_s = cars[PLAYER].s - PLAYER_Z;
    compute_offsets();
    place_sprites();
    int far = (int)floorf(bg_scroll * 0.5f);
    int near = (int)floorf(bg_scroll);
    draw_sky_strip(0);
    if (sky_dirty || far != drawn_far || near != drawn_near || message != drawn_message) {
        for (int y0 = STRIP; y0 < HORIZON; y0 += STRIP) {
            draw_sky_strip(y0);
        }
        drawn_far = far;
        drawn_near = near;
        drawn_message = message;
        sky_dirty = false;
    }
    for (int y0 = HORIZON; y0 < SCREEN_H; y0 += STRIP) {
        draw_road_strip(y0);
    }
}

/* ---- Driving ---- */

static bool touch_held;
static bool touch_secret; /* this press began in the secret corner */
static int touch_missed;
static int16_t touch_x, touch_y; /* the latest reading of the press */

typedef struct {
    int steer; /* -1 left, 1 right or 0 */
    int pedal; /* on the map only */
} controls_t;

/* Over the horizon, the left or right half of the screen steers. On the
   map, the left half is the pedals (gas above PEDAL_SPLIT_Y, brake below)
   and the right half steers (left of STEER_SPLIT_X, right beyond it). The
   panel reads one touch at a time, so pedals and steering take turns. A
   press that starts in the secret corner toggles the auto-win once and does
   nothing else. */
static controls_t read_controls(void)
{
    cyd_touch_point_t point;
    if (cyd_touch_read(&point)) {
        touch_missed = 0;
        touch_x = point.x;
        touch_y = point.y;
        if (!touch_held) {
            touch_held = true;
            touch_secret = in_secret_corner(&point);
            if (touch_secret) {
                toggle_rigged();
            }
        }
    } else if (touch_held && ++touch_missed >= RELEASE_FRAMES) {
        touch_held = false;
        touch_secret = false;
    }

    controls_t controls = {0, PEDAL_NONE};
    if (touch_held && !touch_secret) {
        if (view == VIEW_HORIZON) {
            controls.steer = touch_x < CENTER_X ? -1 : 1;
        } else if (touch_x < CENTER_X) {
            controls.pedal = touch_y < PEDAL_SPLIT_Y ? PEDAL_GAS : PEDAL_BRAKE;
        } else {
            controls.steer = touch_x < STEER_SPLIT_X ? -1 : 1;
        }
    }
    shown_pedal = controls.pedal;
    return controls;
}

static void approach(float *value, float target, float up, float down)
{
    if (*value < target) {
        *value = fminf(target, *value + up);
    } else {
        *value = fmaxf(target, *value - down);
    }
}

static void move_player(float dt, controls_t controls)
{
    car_t *player = &cars[PLAYER];
    bool offroad = fabsf(player->x) > OFFROAD_EDGE;
    if (view == VIEW_HORIZON || player->finish_time >= 0) {
        /* Over the horizon, and after the flag, the car drives itself. */
        float limit = player->finish_time >= 0 ? COAST_SPEED
                      : offroad                ? OFFROAD_SPEED
                                               : TOP_SPEED;
        limit = fminf(limit, safe_speed_at(player->s));
        approach(&player->v, limit, ACCELERATION * dt,
                 (offroad ? OFFROAD_BRAKING : BRAKING) * dt);
    } else {
        /* On the map, the player's pedals; corners taken too fast run wide. */
        float limit = offroad ? OFFROAD_SPEED : TOP_SPEED;
        if (player->v > limit) {
            player->v = fmaxf(limit, player->v - OFFROAD_BRAKING * dt);
        } else if (controls.pedal == PEDAL_GAS) {
            player->v = fminf(limit, player->v + ACCELERATION * dt);
        }
        if (controls.pedal == PEDAL_BRAKE) {
            player->v = fmaxf(0, player->v - BRAKING * dt);
        } else if (controls.pedal == PEDAL_NONE) {
            player->v = fmaxf(0, player->v - COAST_DRAG * dt);
        }
    }
    float c = curvature_at(player->s);
    float steer_rate = STEER_RATE * (0.3f + 0.7f * player->v / TOP_SPEED);
    player->x += (controls.steer * steer_rate - c * player->v * player->v * GRIP) * dt;
    player->x = fmaxf(-GRASS_EDGE, fminf(GRASS_EDGE, player->x));
    player->s += player->v * dt;
    bg_scroll += c * player->v * PARALLAX * dt;
    if (player->finish_time < 0 && player->s >= track_length) {
        player->finish_time = race_time;
        ESP_LOGI(TAG, "Finished P%d in %.2f s", car_place(player), race_time);
    }
}

static void move_rival(car_t *car, float dt)
{
    const car_t *player = &cars[PLAYER];
    float safe = safe_speed_at(car->s);
    float target = car->pace * safe;
    float progress = player->s / track_length;
    if (rigged && progress > RIG_FROM) {
        /* Close up to a lead over the player that grows toward the finish,
           carrying a little more speed than is safe through the corners. */
        float ramp = fminf(1, (progress - RIG_FROM) / RIG_RAMP);
        float wanted = player->s + car->lead * ramp;
        float gap = wanted - car->s;
        if (gap > 0) {
            target = fmaxf(target, fminf(RIG_SPEEDUP * safe, player->v + 1.5f * gap));
        }
        if (progress > RIG_CLAMP && car->s < wanted) {
            car->s = wanted;
        }
    }
    approach(&car->v, target, ACCELERATION * dt, BRAKING * dt);
    car->s += car->v * dt;

    /* Wander between lanes. Coming up on the player, pick the side with more
       road to pass on and keep to it for a moment: rivals steer at half the
       player's rate, so the player can move across and block. */
    car->lane_timer -= dt;
    car->pass_timer -= dt;
    if (car->lane_timer <= 0) {
        static const float lanes[] = {-0.5f, 0, 0.5f};
        car->lane = lanes[esp_random() % 3];
        car->lane_timer = random_range(2, 5);
    }
    float behind = player->s - car->s;
    if (behind > -0.5f && behind < 8 && fabsf(car->x - player->x) < 0.6f &&
        car->pass_timer <= 0) {
        car->lane = player->x > 0 ? -0.6f : 0.6f;
        car->pass_timer = PASS_REACTION;
        car->lane_timer = fmaxf(car->lane_timer, 2);
    }
    approach(&car->x, car->lane, RIVAL_STEER * dt, RIVAL_STEER * dt);

    if (car->finish_time < 0 && car->s >= track_length) {
        car->finish_time = race_time;
        ESP_LOGI(TAG, "%s finished in %.2f s", car->name, race_time);
    }
}

/* Cars touching: side by side they push apart; the player running into the
   back of a rival is held behind it; and a rival running into the back of
   the player is held behind it too, so the player can block. Late in a
   rigged race, rivals squeeze past instead. */
static void check_collisions(const float before[CAR_COUNT], float dt)
{
    car_t *player = &cars[PLAYER];
    const float width = CAR_WIDTH * 0.9f;
    bool squeeze = rigged && player->s / track_length > RIG_SQUEEZE;
    for (int i = 0; i < PLAYER; ++i) {
        car_t *car = &cars[i];
        float gap = car->s - player->s;
        float dx = car->x - player->x;
        if (fabsf(gap) >= CAR_LENGTH || fabsf(dx) >= width) {
            continue;
        }
        float away = dx < 0 ? -1 : 1; /* the rival's side of the player */
        if (fabsf(before[i]) < CAR_LENGTH * 0.6f) {
            float push = (width - fabsf(dx)) / 2;
            car->x += away * push;
            player->x -= away * push;
            player->v *= 1 - 0.5f * dt;
        } else if (before[i] > 0) {
            player->s = car->s - CAR_LENGTH;
            player->v = fminf(player->v, car->v * 0.75f);
            player->x -= away * 0.06f;
        } else if (squeeze) {
            car->x += away * 3 * dt;
        } else {
            car->s = player->s - CAR_LENGTH;
            car->v = fminf(car->v, player->v);
        }
    }
}

static void update(float dt, controls_t controls)
{
    float before[CAR_COUNT];
    for (int i = 0; i < CAR_COUNT; ++i) {
        before[i] = cars[i].s - cars[PLAYER].s;
    }
    race_time += dt;
    for (int i = 0; i < PLAYER; ++i) {
        move_rival(&cars[i], dt);
    }
    move_player(dt, controls);
    check_collisions(before, dt);
}

/* Waits for the rest of the frame, but always yields so the idle task runs. */
static void end_frame(TickType_t *last_frame)
{
    TickType_t now = xTaskGetTickCount();
    if (now - *last_frame < pdMS_TO_TICKS(FRAME_MS)) {
        vTaskDelayUntil(last_frame, pdMS_TO_TICKS(FRAME_MS));
    } else {
        *last_frame = now;
        vTaskDelay(1);
    }
}

static void run_race(void)
{
    static const char *const COUNTDOWN[] = {"3", "2", "1"};
    reset_cars();
    touch_held = false;
    TickType_t last_frame = xTaskGetTickCount();

    for (int i = 0; i < 3; ++i) {
        message = COUNTDOWN[i];
        message_color = CYD_COLOR_RED;
        int64_t until = esp_timer_get_time() + 800000;
        while (esp_timer_get_time() < until) {
            read_controls();
            render();
            ++frame_count;
            end_frame(&last_frame);
        }
    }

    static const char GO[] = "GO";
    message = GO;
    message_color = COLOR_GREEN;
    int64_t last = esp_timer_get_time();
    float finished_for = 0;
    while (finished_for < FINISH_SECONDS) {
        int64_t now = esp_timer_get_time();
        float dt = fminf(0.08f, (float)(now - last) / 1e6f);
        last = now;
        update(dt, read_controls());
        if (message == GO && race_time > 1) {
            message = NULL;
        }
        if (view == VIEW_HORIZON && race_time >= HORIZON_SECONDS) {
            view = VIEW_MAP;
            draw_map();
        }
        if (cars[PLAYER].finish_time >= 0) {
            message = "FINISH";
            message_color = CYD_COLOR_WHITE;
            finished_for += dt;
        }
        render();
        ++frame_count;
        end_frame(&last_frame);
    }
    message = NULL;
}

/* ---- Screens ---- */

/* Waits for a tap and returns where it began. */
static void wait_for_tap(cyd_touch_point_t *tap)
{
    cyd_touch_point_t point;
    while (cyd_touch_read(&point)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    while (!cyd_touch_read(tap)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (int missed = 0; missed < RELEASE_FRAMES;) {
        missed = cyd_touch_read(&point) ? 0 : missed + 1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void draw_indicator(void)
{
    cyd_fill_rect(INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE,
                  rigged ? CYD_COLOR_BLACK : COLOR_INDICATOR);
}

static void centered_row(int row, uint16_t color, const char *text)
{
    char line[CYD_TEXT_COLUMNS + 1];
    int length = (int)strlen(text);
    int pad = length < CYD_TEXT_COLUMNS ? (CYD_TEXT_COLUMNS - length) / 2 : 0;
    snprintf(line, sizeof(line), "%*s%s", pad, "", text);
    cyd_text_row(row, color, CYD_COLOR_BLACK, line);
}

/* A red italic F1 mark for the splash screen, drawn from shapes. */
#define LOGO_W 150
#define LOGO_H 40
#define LOGO_X ((SCREEN_W - LOGO_W) / 2)
#define LOGO_Y 6
#define LOGO_SLANT 0.45f
#define COLOR_LOGO RGB565(225, 6, 0)

static bool logo_pixel(int x, int y)
{
    if (x < 0 || x >= LOGO_W || y < 0 || y >= LOGO_H) {
        return false;
    }
    /* u runs along the letters, unslanted. */
    float u = x - (LOGO_H - 1 - y) * LOGO_SLANT;
    if (u < 0) {
        return false;
    }
    if (u < 12 && y < 12 && (12 - u) * (12 - u) + (12 - y) * (12 - y) > 144) {
        return false; /* the F's rounded top-left corner */
    }
    bool stem = u < 22;
    bool top_bar = y < 12 && u < 96;
    bool middle_bar = y >= 17 && y < 28 && u < 70;
    bool one = u >= 104 && u < 126;
    return stem || top_bar || middle_bar || one;
}

static void draw_logo(void)
{
    for (int y0 = LOGO_Y; y0 < LOGO_Y + LOGO_H; y0 += STRIP) {
        for (int r = 0; r < STRIP; ++r) {
            for (int x = 0; x < SCREEN_W; ++x) {
                strip[r * SCREEN_W + x] =
                    logo_pixel(x - LOGO_X, y0 + r - LOGO_Y) ? COLOR_LOGO : CYD_COLOR_BLACK;
            }
        }
        cyd_draw_bitmap(0, y0, SCREEN_W, STRIP, strip);
    }
}

#define START_ROW 9
#define START_FIRST 5
#define START_LAST 11

static void show_splash(void)
{
    cyd_fill_rect(0, 0, SCREEN_W, SCREEN_H, CYD_COLOR_BLACK);
    draw_logo();
    centered_row(2, CYD_COLOR_WHITE, "COTA GRAND PRIX");

    /* The player's car at three times its size. */
    sprites[0] = (sprite_t){.x = (SCREEN_W - 3 * SPRITE_W) / 2, .y = 80, .w = 3 * SPRITE_W,
                            .h = 3 * SPRITE_H, .pixels = cars[PLAYER].pixels};
    sprite_count = 1;
    for (int y0 = 80; y0 < 80 + 3 * SPRITE_H; y0 += STRIP) {
        memset(strip, 0, sizeof(strip));
        strip_sprites(y0);
        cyd_draw_bitmap(0, y0, SCREEN_W, STRIP, strip);
    }

    centered_row(6, CYD_COLOR_CYAN, "Tap left or right");
    centered_row(7, CYD_COLOR_CYAN, "side to steer");

    char label[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    snprintf(label, sizeof(label), "%*s START", START_FIRST, "");
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        bool button = i >= START_FIRST && i <= START_LAST;
        foreground[i] = CYD_COLOR_WHITE;
        background[i] = button ? RGB565(200, 0, 0) : CYD_COLOR_BLACK;
    }
    cyd_draw_cells(START_ROW, label, foreground, background);
    draw_indicator();

    while (true) {
        cyd_touch_point_t tap;
        wait_for_tap(&tap);
        if (in_secret_corner(&tap)) {
            toggle_rigged();
            draw_indicator();
            continue;
        }
        const int slop = 12;
        if (tap.x >= START_FIRST * CYD_FONT_WIDTH - slop &&
            tap.x < (START_LAST + 1) * CYD_FONT_WIDTH + slop &&
            tap.y >= START_ROW * CYD_FONT_HEIGHT - slop) {
            return;
        }
    }
}

static void show_results(void)
{
    int order[CAR_COUNT];
    float times[CAR_COUNT];
    for (int i = 0; i < CAR_COUNT; ++i) {
        const car_t *car = &cars[i];
        times[i] = car->finish_time >= 0
                       ? car->finish_time
                       : race_time + (track_length - car->s) / fmaxf(car->v, 1);
        order[i] = i;
    }
    if (rigged) {
        /* Hamilton, Kimi, Verstappen, then the player, at the player's own
           time; the rivals' times are kept in that order. */
        for (int k = CAR_COUNT - 2; k >= 0; --k) {
            times[k] = fminf(times[k], times[k + 1] - 0.1f);
        }
    } else {
        for (int i = 1; i < CAR_COUNT; ++i) {
            int index = order[i];
            int j = i - 1;
            while (j >= 0 && times[order[j]] > times[index]) {
                order[j + 1] = order[j];
                --j;
            }
            order[j + 1] = index;
        }
    }

    cyd_fill_rect(0, 0, SCREEN_W, SCREEN_H, CYD_COLOR_BLACK);
    centered_row(0, CYD_COLOR_YELLOW, "CHEQUERED FLAG");
    int place = 0;
    for (int k = 0; k < CAR_COUNT; ++k) {
        const car_t *car = &cars[order[k]];
        float time = times[order[k]];
        char gap[16];
        if (k == 0) {
            snprintf(gap, sizeof(gap), "%4.1f", fminf(time, 99.9f));
        } else {
            float behind = time - times[order[0]];
            if (behind < 9.95f) {
                snprintf(gap, sizeof(gap), "+%.1f", behind);
            } else {
                snprintf(gap, sizeof(gap), "+%2d", (int)fminf(behind + 0.5f, 99));
            }
        }
        char line[48];
        snprintf(line, sizeof(line), "%d %-10s %4s", k + 1, car->name, gap);
        uint16_t foreground[CYD_TEXT_COLUMNS];
        uint16_t background[CYD_TEXT_COLUMNS];
        for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
            foreground[i] = i >= 2 && i < 12 ? car->label : CYD_COLOR_WHITE;
            background[i] = order[k] == PLAYER ? RGB565(60, 50, 0) : CYD_COLOR_BLACK;
        }
        cyd_draw_cells(2 + k, line, foreground, background);
        if (order[k] == PLAYER) {
            place = k + 1;
        }
    }

    char text[32];
    if (place == 1) {
        centered_row(7, COLOR_GREEN, "YOU WIN!");
    } else {
        snprintf(text, sizeof(text), "YOU FINISHED P%d", place);
        centered_row(7, CYD_COLOR_WHITE, text);
    }
    snprintf(text, sizeof(text), "TIME %.2f S", times[PLAYER]);
    centered_row(8, CYD_COLOR_GRAY, text);
    centered_row(9, CYD_COLOR_YELLOW, "tap to go on");
    draw_indicator();
    ESP_LOGI(TAG, "Result: P%d of %d, %s", place, CAR_COUNT,
             rigged ? "auto-win on" : "fair");

    while (true) {
        cyd_touch_point_t tap;
        wait_for_tap(&tap);
        if (!in_secret_corner(&tap)) {
            return;
        }
        toggle_rigged();
        draw_indicator();
    }
}

static void game_task(void *argument)
{
    (void)argument;
    load_rigged();
    build_track();
    build_sprites();
    build_scenery();
    while (true) {
        show_splash();
        run_race();
        show_results();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_init());
    BaseType_t result = xTaskCreate(game_task, "game", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
