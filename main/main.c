#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Cat & Mouse: a small maze game in the manner of PAC-MAN. The mouse eats
   the cheese while three cats chase it; a big cheese lets it chase them
   back to their basket for a while. Steer with the W, A, S and D keys in
   the bottom-right corner.

   Everything is drawn straight to the panel without a frame buffer. The
   maze is 20x14 tiles of 16 pixels under a 16-pixel score bar, and each
   frame redraws only the small boxes around the mouse and the cats,
   composed in RAM first so nothing flickers. */

static const char *TAG = "catmouse";

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define TILE 16
#define COLS 20
#define ROWS 14
#define HUD_HEIGHT 16
#define MAZE_WIDTH (COLS * TILE)
#define MAZE_HEIGHT (ROWS * TILE)

#define FRAME_MS 40 /* 25 frames per second */
#define FRAMES(seconds) ((int)((seconds) * 1000 / FRAME_MS))

/* Speeds in eighths of a pixel per frame. */
#define MOUSE_SPEED 14
#define CAT_SPEED 11
#define CAT_SPEED_MAX 15
#define SCARED_SPEED 7
#define EYES_SPEED 24

#define START_LIVES 3
#define CHEESE_POINTS 10
#define BIG_CHEESE_POINTS 50
#define CAT_POINTS 200
/* Closer than this on both axes, a cat catches the mouse. */
#define CATCH_DISTANCE 11

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_WALL RGB565(20, 30, 150)
#define COLOR_WALL_EDGE RGB565(90, 130, 255)
#define COLOR_CHEESE RGB565(255, 220, 60)
#define COLOR_BIG_CHEESE RGB565(255, 190, 0)
#define COLOR_CHEESE_HOLE RGB565(200, 130, 0)
#define COLOR_DOOR RGB565(255, 150, 200)
#define COLOR_MOUSE RGB565(170, 170, 180)
#define COLOR_PINK RGB565(255, 140, 170)
#define COLOR_SCARED RGB565(40, 60, 255)

/* '#' wall, '.' cheese, 'o' big cheese, '-' the basket's door, ' ' empty.
   Row 7 runs off both sides: leaving one side enters the other. The basket
   (row 7, columns 8-11) is closed; cats leave it by appearing on the tile
   above its door. The WASD pad covers rows 11-13 of columns 14-19, so
   those are wall and the bottom-right corner is reshaped around them,
   with its big cheese moved up to row 9. */
static const char *const MAZE_LAYOUT[ROWS] = {
    "####################",
    "#o.......##.......o#",
    "#.##.###.##.###.##.#",
    "#..................#",
    "#.##.#.######.#.##.#",
    "#....#........#....#",
    "####.#.##--##.#.####",
    "    .#.#    #.#.    ",
    "####.#.######.#.####",
    "#........##....o####",
    "#.##.###.##.########",
    "#o............######",
    "###......##...######",
    "####################",
};

#define MOUSE_START_COL 9
#define MOUSE_START_ROW 11
#define EXIT_COL 9 /* where cats appear when they leave the basket */
#define EXIT_ROW 5
#define BASKET_Y (7 * TILE)

enum { DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT, DIR_NONE };
static const int DX[4] = {0, -1, 0, 1};
static const int DY[4] = {-1, 0, 1, 0};

static int opposite(int dir)
{
    return dir == DIR_NONE ? DIR_NONE : (dir + 2) % 4;
}

typedef struct {
    int x, y;         /* top-left corner, in maze pixels */
    int old_x, old_y; /* where it was drawn last */
    int dir;
    int sub; /* eighths of a pixel still to move */
} actor_t;

typedef enum { CAT_BASKET, CAT_HUNT, CAT_SCARED, CAT_EYES } cat_mode_t;

typedef struct {
    actor_t actor;
    cat_mode_t mode;
    int timer;    /* frames left in the basket */
    int basket_x; /* its place in the basket */
    uint16_t color;
    int home_col, home_row; /* the corner it wanders to when not hunting */
} cat_t;

#define CAT_COUNT 3

static char maze[ROWS][COLS + 1];
static int cheese_left;
static actor_t mouse;
static int wanted_dir; /* the way the player last asked to go */
static int mouse_facing;
static cat_t cats[CAT_COUNT];
static int scared_frames; /* frames left of the big cheese */
static int cats_caught;   /* during this big cheese, for doubling points */
static int mode_frames;   /* into the wander/hunt cycle */
static bool wandering;
static bool big_cheese_shown = true; /* big cheese blinks */
static bool walls_flash;             /* walls drawn white, when a level is cleared */
static int frame_count;
static uint32_t score;
static uint32_t high_score;
static int lives;
static int level;

/* ---- Sprites ----

   16x16, one character per pixel. The mouse is drawn facing right and
   turned for the other directions; 'X' is each cat's own color. */

static const char *const MOUSE_SPRITE[TILE] = {
    "................",
    "........PP......",
    ".......PPPP.....",
    "......GGPPGG....",
    "....GGGGGGGGG...",
    "...GGGGGGGGGGG..",
    "T.GGGGGGGGGGKGG.",
    ".TGGGGGGGGGGGGGN",
    ".TGGGGGGGGGGGGGN",
    "T.GGGGGGGGGGKGG.",
    "...GGGGGGGGGGG..",
    "....GGGGGGGGG...",
    "......GGPPGG....",
    ".......PPPP.....",
    "........PP......",
    "................",
};

static const char *const CAT_SPRITE[TILE] = {
    "................",
    ".X............X.",
    ".XX..........XX.",
    ".XPX........XPX.",
    ".XXXXXXXXXXXXXX.",
    "XXXXXXXXXXXXXXXX",
    "XXXWWXXXXXXWWXXX",
    "XXXWKXXXXXXKWXXX",
    "XXXXXXXXXXXXXXXX",
    "XXXXXXXPPXXXXXXX",
    "WWWXXXXXXXXXXWWW",
    "XXXWWXXXXXXWWXXX",
    ".XXXXXXXXXXXXXX.",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "................",
};

/* The mouse sprite's pixel at (x, y) when facing dir. */
static char mouse_sprite_at(int dir, int x, int y)
{
    switch (dir) {
    case DIR_LEFT:
        return MOUSE_SPRITE[y][TILE - 1 - x];
    case DIR_UP:
        return MOUSE_SPRITE[x][TILE - 1 - y];
    case DIR_DOWN:
        return MOUSE_SPRITE[TILE - 1 - x][y];
    default:
        return MOUSE_SPRITE[y][x];
    }
}

static bool mouse_pixel(int dir, int x, int y, uint16_t *color)
{
    switch (mouse_sprite_at(dir, x, y)) {
    case 'G':
        *color = COLOR_MOUSE;
        return true;
    case 'P':
    case 'N':
    case 'T':
        *color = COLOR_PINK;
        return true;
    case 'K':
        *color = COLOR_BLACK;
        return true;
    default:
        return false;
    }
}

static bool cat_pixel(const cat_t *cat, int x, int y, uint16_t *color)
{
    char c = CAT_SPRITE[y][x];
    if (c == '.') {
        return false;
    }
    if (cat->mode == CAT_EYES) {
        /* Caught: only its eyes run home. */
        if (c != 'W' && c != 'K') {
            return false;
        }
        *color = c == 'W' ? COLOR_WHITE : COLOR_SCARED;
        return true;
    }
    uint16_t body = cat->color;
    if (cat->mode == CAT_SCARED) {
        /* Flashes white as the big cheese wears off. */
        body = scared_frames < FRAMES(2) && (frame_count / 5) % 2 ? COLOR_WHITE : COLOR_SCARED;
    }
    switch (c) {
    case 'X':
        *color = body;
        break;
    case 'W':
        *color = COLOR_WHITE;
        break;
    case 'K':
        *color = COLOR_BLACK;
        break;
    default:
        *color = COLOR_PINK;
        break;
    }
    return true;
}

/* ---- The maze ---- */

static bool layout_wall(int col, int row)
{
    return col < 0 || col >= COLS || row < 0 || row >= ROWS || MAZE_LAYOUT[row][col] == '#';
}

/* True when the mouse or a cat may stand on the tile. */
static bool passable(int col, int row)
{
    col = (col + COLS) % COLS;
    return row >= 0 && row < ROWS && maze[row][col] != '#' && maze[row][col] != '-';
}

static uint16_t big_cheese_pixel(int x, int y)
{
    /* A round cheese with two holes, in half-pixel units from the center. */
    int dx = 2 * x - (TILE - 1);
    int dy = 2 * y - (TILE - 1);
    if (dx * dx + dy * dy > 11 * 11) {
        return COLOR_BLACK;
    }
    if ((dx - 4) * (dx - 4) + (dy + 3) * (dy + 3) <= 9 ||
        (dx + 4) * (dx + 4) + (dy - 4) * (dy - 4) <= 6) {
        return COLOR_CHEESE_HOLE;
    }
    return COLOR_BIG_CHEESE;
}

/* The maze's own pixel at (x, y), without the mouse or cats. */
static uint16_t maze_pixel(int x, int y)
{
    int col = x / TILE;
    int row = y / TILE;
    int lx = x % TILE;
    int ly = y % TILE;
    switch (maze[row][col]) {
    case '#': {
        if (walls_flash) {
            return COLOR_WHITE;
        }
        /* A lighter rim along the sides that face a corridor. */
        bool rim = (ly < 3 && !layout_wall(col, row - 1)) ||
                   (ly > TILE - 4 && !layout_wall(col, row + 1)) ||
                   (lx < 3 && !layout_wall(col - 1, row)) ||
                   (lx > TILE - 4 && !layout_wall(col + 1, row));
        return rim ? COLOR_WALL_EDGE : COLOR_WALL;
    }
    case '.':
        return lx >= 6 && lx <= 9 && ly >= 6 && ly <= 9 ? COLOR_CHEESE : COLOR_BLACK;
    case 'o':
        return big_cheese_shown ? big_cheese_pixel(lx, ly) : COLOR_BLACK;
    case '-':
        return ly >= 6 && ly <= 9 ? COLOR_DOOR : COLOR_BLACK;
    default:
        return COLOR_BLACK;
    }
}

/* ---- The WASD pad ----

   Four keys in the bottom-right corner, laid out like a keyboard's: W above
   S, with A and D either side of it. The tiles under the pad are wall, and
   the maze drawing leaves the pad's corner to it. */

#define KEY_WIDTH 32
#define KEY_HEIGHT 24
#define KEY_GLYPH_SCALE 4
#define PAD_WIDTH (3 * KEY_WIDTH)
#define PAD_HEIGHT (2 * KEY_HEIGHT)
#define PAD_X (CYD_LCD_WIDTH - PAD_WIDTH) /* screen and maze x are the same */
#define PAD_Y (CYD_LCD_HEIGHT - PAD_HEIGHT)
#define PAD_MAZE_Y (PAD_Y - HUD_HEIGHT)
/* Touches this close outside the pad still go to the nearest key, for the
   resistive panel's aim. */
#define PAD_SLOP 10
#define COLOR_KEY RGB565(60, 70, 110)
#define COLOR_KEY_PRESSED RGB565(255, 220, 60)

typedef struct {
    uint16_t glyph; /* 3x5, three bits per row (octal) */
    int dir;
    int x, y; /* within the pad */
} pad_key_t;

static const pad_key_t PAD_KEYS[4] = {
    {055775, DIR_UP, KEY_WIDTH, 0},              /* W */
    {025755, DIR_LEFT, 0, KEY_HEIGHT},           /* A */
    {034216, DIR_DOWN, KEY_WIDTH, KEY_HEIGHT},   /* S */
    {065556, DIR_RIGHT, 2 * KEY_WIDTH, KEY_HEIGHT}, /* D */
};

static int pressed_key = -1;

/* A 3x5 glyph's pixel at (x, y), drawn scale times its size. */
static bool glyph_pixel(uint16_t glyph, int scale, int x, int y)
{
    if (x < 0 || x >= 3 * scale || y < 0 || y >= 5 * scale) {
        return false;
    }
    int bits = glyph >> (3 * (4 - y / scale));
    return (bits >> (2 - x / scale)) & 1;
}

/* The pad's pixel at (x, y) within it, false off the keys (the corners
   beside W, which show the wall beneath): each key has a one-pixel gap at
   its right and bottom, a centered letter, and lights while pressed. */
static bool pad_pixel(int x, int y, uint16_t *color)
{
    /* The letter's offset within the key's visible part. */
    const int letter_x = (KEY_WIDTH - 1 - 3 * KEY_GLYPH_SCALE) / 2;
    const int letter_y = (KEY_HEIGHT - 1 - 5 * KEY_GLYPH_SCALE) / 2;
    for (int i = 0; i < 4; ++i) {
        const pad_key_t *key = &PAD_KEYS[i];
        int lx = x - key->x;
        int ly = y - key->y;
        if (lx < 0 || lx >= KEY_WIDTH || ly < 0 || ly >= KEY_HEIGHT) {
            continue;
        }
        bool pressed = i == pressed_key;
        if (lx == KEY_WIDTH - 1 || ly == KEY_HEIGHT - 1) {
            *color = COLOR_BLACK;
        } else if (glyph_pixel(key->glyph, KEY_GLYPH_SCALE, lx - letter_x, ly - letter_y)) {
            *color = pressed ? COLOR_BLACK : COLOR_WHITE;
        } else {
            *color = pressed ? COLOR_KEY_PRESSED : COLOR_KEY;
        }
        return true;
    }
    return false;
}

/* ---- Drawing ---- */

#define REGION_SIZE 32
static uint16_t region[REGION_SIZE * REGION_SIZE];

static bool overlaps(int x0, int y0, int x1, int y1, const actor_t *actor)
{
    return actor->x < x1 && actor->x + TILE > x0 && actor->y < y1 && actor->y + TILE > y0;
}

/* Draws maze pixels [x0, x1) x [y0, y1) with the cats and the mouse on top. */
static void draw_region(int x0, int y0, int x1, int y1)
{
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > MAZE_WIDTH ? MAZE_WIDTH : x1;
    y1 = y1 > MAZE_HEIGHT ? MAZE_HEIGHT : y1;
    int width = x1 - x0;
    int height = y1 - y0;
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width > REGION_SIZE || height > REGION_SIZE) {
        int middle_x = width > REGION_SIZE ? x0 + width / 2 : x1;
        int middle_y = height > REGION_SIZE ? y0 + height / 2 : y1;
        draw_region(x0, y0, middle_x, middle_y);
        draw_region(middle_x, y0, x1, middle_y);
        draw_region(x0, middle_y, middle_x, y1);
        draw_region(middle_x, middle_y, x1, y1);
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            region[(y - y0) * width + (x - x0)] = maze_pixel(x, y);
        }
    }
    for (int i = 0; i <= CAT_COUNT; ++i) {
        /* Cats first, so the mouse stays on top. */
        const actor_t *actor = i < CAT_COUNT ? &cats[i].actor : &mouse;
        if (!overlaps(x0, y0, x1, y1, actor)) {
            continue;
        }
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                int sx = x - actor->x;
                int sy = y - actor->y;
                uint16_t color;
                if (sx < 0 || sx >= TILE || sy < 0 || sy >= TILE) {
                    continue;
                }
                bool drawn = i < CAT_COUNT ? cat_pixel(&cats[i], sx, sy, &color)
                                           : mouse_pixel(mouse_facing, sx, sy, &color);
                if (drawn) {
                    region[(y - y0) * width + (x - x0)] = color;
                }
            }
        }
    }
    /* The pad's corner is kept out of the maze. */
    for (int y = y0 > PAD_MAZE_Y ? y0 : PAD_MAZE_Y; y < y1; ++y) {
        for (int x = x0 > PAD_X ? x0 : PAD_X; x < x1; ++x) {
            uint16_t color;
            if (pad_pixel(x - PAD_X, y - PAD_MAZE_Y, &color)) {
                region[(y - y0) * width + (x - x0)] = color;
            }
        }
    }
    ESP_ERROR_CHECK(cyd_draw_bitmap(x0, HUD_HEIGHT + y0, width, height, region));
}

static void draw_pad(void)
{
    draw_region(PAD_X, PAD_MAZE_Y, MAZE_WIDTH, MAZE_HEIGHT);
}

static void draw_tile(int col, int row)
{
    draw_region(col * TILE, row * TILE, (col + 1) * TILE, (row + 1) * TILE);
}

static void draw_maze(void)
{
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            draw_tile(col, row);
        }
    }
}

/* Redraws where an actor was and where it is now: one box when they
   overlap, two after a jump through the tunnel or into the basket. */
static void redraw_actor(actor_t *actor)
{
    int ox = actor->old_x;
    int oy = actor->old_y;
    if (abs(ox - actor->x) < TILE && abs(oy - actor->y) < TILE) {
        int x0 = ox < actor->x ? ox : actor->x;
        int y0 = oy < actor->y ? oy : actor->y;
        int x1 = (ox > actor->x ? ox : actor->x) + TILE;
        int y1 = (oy > actor->y ? oy : actor->y) + TILE;
        draw_region(x0, y0, x1, y1);
    } else {
        draw_region(ox, oy, ox + TILE, oy + TILE);
        draw_region(actor->x, actor->y, actor->x + TILE, actor->y + TILE);
    }
    actor->old_x = actor->x;
    actor->old_y = actor->y;
}

/* 3x5 digits, three bits per row (octal), drawn at twice their size. */
static const uint16_t DIGITS[10] = {
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
};

static bool digit_pixel(const char *text, int left, int x, int y)
{
    int index = (x - left) / 8;
    int cx = (x - left) % 8;
    int cy = y - 3;
    if (x < left || index >= (int)strlen(text)) {
        return false;
    }
    return glyph_pixel(DIGITS[text[index] - '0'], 2, cx, cy);
}

#define LEVEL_ICON_X 136
#define LEVEL_TEXT_X (LEVEL_ICON_X + TILE + 2)

/* The score at the left, the level by a big cheese, the lives at the right. */
static uint16_t hud_pixel(int x, int y, const char *score_text, const char *level_text)
{
    if (digit_pixel(score_text, 4, x, y) || digit_pixel(level_text, LEVEL_TEXT_X, x, y)) {
        return COLOR_WHITE;
    }
    if (x >= LEVEL_ICON_X && x < LEVEL_ICON_X + TILE) {
        return big_cheese_pixel(x - LEVEL_ICON_X, y);
    }
    int from_right = CYD_LCD_WIDTH - 1 - x;
    int life = from_right / (TILE + 2);
    int lx = TILE - 1 - from_right % (TILE + 2);
    uint16_t color;
    if (life < lives && lx >= 0 && mouse_pixel(DIR_RIGHT, lx, y, &color)) {
        return color;
    }
    return COLOR_BLACK;
}

static void draw_hud(void)
{
    char score_text[12];
    char level_text[12];
    snprintf(score_text, sizeof(score_text), "%u", (unsigned)(score % 1000000));
    snprintf(level_text, sizeof(level_text), "%d", level % 100);
    for (int left = 0; left < CYD_LCD_WIDTH; left += REGION_SIZE) {
        for (int y = 0; y < HUD_HEIGHT; ++y) {
            for (int x = 0; x < REGION_SIZE; ++x) {
                region[y * REGION_SIZE + x] = hud_pixel(left + x, y, score_text, level_text);
            }
        }
        ESP_ERROR_CHECK(cyd_draw_bitmap(left, 0, REGION_SIZE, HUD_HEIGHT, region));
    }
}

static void draw_big_cheeses(void)
{
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            if (maze[row][col] == 'o') {
                draw_tile(col, row);
            }
        }
    }
}

/* ---- Text screens ---- */

/* Text centered in the 17 columns of one row. */
static void text_row(int row, uint16_t color, const char *text)
{
    char line[40];
    int length = (int)strnlen(text, CYD_TEXT_COLUMNS);
    int pad = (CYD_TEXT_COLUMNS - length) / 2;
    snprintf(line, sizeof(line), "%*s%.*s", pad, "", length, text);
    ESP_ERROR_CHECK(cyd_text_row(row, color, COLOR_BLACK, line));
}

static void clear_screen(void)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, COLOR_BLACK));
}

/* Draws the mouse, or a cat, on a black background anywhere on the screen. */
static void draw_sprite(int x, int y, const cat_t *cat)
{
    for (int sy = 0; sy < TILE; ++sy) {
        for (int sx = 0; sx < TILE; ++sx) {
            uint16_t color = COLOR_BLACK;
            if (cat != NULL) {
                cat_pixel(cat, sx, sy, &color);
            } else {
                mouse_pixel(DIR_RIGHT, sx, sy, &color);
            }
            region[sy * TILE + sx] = color;
        }
    }
    ESP_ERROR_CHECK(cyd_draw_bitmap(x, y, TILE, TILE, region));
}

/* Waits for a finger to land and lift. */
static void wait_for_tap(void)
{
    cyd_touch_point_t point;
    while (cyd_touch_read(&point)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    while (!cyd_touch_read(&point)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (cyd_touch_read(&point)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void show_title(void)
{
    static const cat_t title_cat = {.mode = CAT_HUNT, .color = RGB565(255, 140, 30)};
    char high[32];
    snprintf(high, sizeof(high), "HIGH %u", (unsigned)high_score);
    clear_screen();
    text_row(1, CYD_COLOR_YELLOW, "CAT & MOUSE");
    text_row(3, CYD_COLOR_WHITE, "Eat the cheese,");
    text_row(4, CYD_COLOR_WHITE, "dodge the cats.");
    text_row(5, CYD_COLOR_WHITE, "Big cheese lets");
    text_row(6, CYD_COLOR_WHITE, "you chase them.");
    text_row(7, CYD_COLOR_CYAN, "Steer: W A S D");
    text_row(8, CYD_COLOR_GRAY, high);
    text_row(9, CYD_COLOR_YELLOW, "tap to play");
    /* A mouse running from a cat, either side of the title. */
    draw_sprite(12, CYD_FONT_HEIGHT + 4, NULL);
    draw_sprite(CYD_LCD_WIDTH - 12 - TILE, CYD_FONT_HEIGHT + 4, &title_cat);
    wait_for_tap();
}

static void load_high_score(void)
{
    nvs_handle_t nvs;
    /* Never erase NVS here: it holds the launcher's settings too. */
    if (nvs_flash_init() == ESP_OK && nvs_open("catmouse", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "high", &high_score);
        nvs_close(nvs);
    }
}

static void save_high_score(void)
{
    nvs_handle_t nvs;
    if (nvs_open("catmouse", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "high", high_score);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void show_game_over(void)
{
    char line[32];
    bool record = score > high_score;
    if (record) {
        high_score = score;
        save_high_score();
    }
    ESP_LOGI(TAG, "Game over: score %u, level %d", (unsigned)score, level);
    clear_screen();
    text_row(2, CYD_COLOR_RED, "GAME OVER");
    snprintf(line, sizeof(line), "SCORE %u", (unsigned)score);
    text_row(4, CYD_COLOR_WHITE, line);
    snprintf(line, sizeof(line), "LEVEL %d", level);
    text_row(5, CYD_COLOR_WHITE, line);
    if (record) {
        text_row(7, CYD_COLOR_YELLOW, "NEW HIGH SCORE!");
    } else {
        snprintf(line, sizeof(line), "HIGH %u", (unsigned)high_score);
        text_row(7, CYD_COLOR_GRAY, line);
    }
    text_row(9, CYD_COLOR_YELLOW, "tap to play");
    vTaskDelay(pdMS_TO_TICKS(1000));
    wait_for_tap();
}

/* ---- Moving ---- */

static bool at_tile_center(const actor_t *actor)
{
    return actor->x % TILE == 0 && actor->y % TILE == 0;
}

/* Only meaningful at a tile center. */
static bool can_go(const actor_t *actor, int dir)
{
    return dir != DIR_NONE && passable(actor->x / TILE + DX[dir], actor->y / TILE + DY[dir]);
}

static void step(actor_t *actor)
{
    actor->x += DX[actor->dir];
    actor->y += DY[actor->dir];
    if (actor->x < 0) {
        actor->x = (COLS - 1) * TILE;
    } else if (actor->x > (COLS - 1) * TILE) {
        actor->x = 0;
    }
}

/* Whole pixels to move this frame at speed, keeping the remainder. */
static int pixels_this_frame(actor_t *actor, int speed)
{
    actor->sub += speed;
    int pixels = actor->sub / 8;
    actor->sub %= 8;
    return pixels;
}

static void place(actor_t *actor, int x, int y, int dir)
{
    actor->x = x;
    actor->y = y;
    actor->dir = dir;
    actor->sub = 0;
}

static void scare_cats(void)
{
    scared_frames = FRAMES(6) - (level - 1) * FRAMES(1);
    if (scared_frames < FRAMES(2)) {
        scared_frames = FRAMES(2);
    }
    cats_caught = 0;
    for (int i = 0; i < CAT_COUNT; ++i) {
        if (cats[i].mode == CAT_HUNT || cats[i].mode == CAT_SCARED) {
            cats[i].mode = CAT_SCARED;
            cats[i].actor.dir = opposite(cats[i].actor.dir);
        }
    }
}

static void eat(int col, int row)
{
    char *tile = &maze[row][col];
    if (*tile == '.') {
        score += CHEESE_POINTS;
    } else if (*tile == 'o') {
        score += BIG_CHEESE_POINTS;
        scare_cats();
    } else {
        return;
    }
    *tile = ' ';
    --cheese_left;
    draw_hud();
}

static void move_mouse(void)
{
    int pixels = pixels_this_frame(&mouse, MOUSE_SPEED);
    for (int i = 0; i < pixels; ++i) {
        /* Turning back works anywhere; other turns wait for a tile center. */
        if (wanted_dir != DIR_NONE && wanted_dir == opposite(mouse.dir)) {
            mouse.dir = wanted_dir;
        }
        if (at_tile_center(&mouse)) {
            eat(mouse.x / TILE, mouse.y / TILE);
            if (can_go(&mouse, wanted_dir)) {
                mouse.dir = wanted_dir;
            }
            if (!can_go(&mouse, mouse.dir)) {
                mouse.dir = DIR_NONE;
            }
        }
        if (mouse.dir == DIR_NONE) {
            mouse.sub = 0;
            break;
        }
        step(&mouse);
        mouse_facing = mouse.dir;
    }
}

/* The tile a cat heads for. Hunting, the orange cat goes for the mouse,
   the white one for four tiles ahead of it, and the pink one for the
   mouse only from afar, else its corner. */
static void cat_target(int index, int *col, int *row)
{
    const cat_t *cat = &cats[index];
    int mouse_col = (mouse.x + TILE / 2) / TILE;
    int mouse_row = (mouse.y + TILE / 2) / TILE;
    if (cat->mode == CAT_EYES) {
        *col = EXIT_COL;
        *row = EXIT_ROW;
    } else if (wandering) {
        *col = cat->home_col;
        *row = cat->home_row;
    } else if (index == 1 && mouse.dir != DIR_NONE) {
        *col = mouse_col + 4 * DX[mouse.dir];
        *row = mouse_row + 4 * DY[mouse.dir];
    } else if (index == 2) {
        int dc = mouse_col - cat->actor.x / TILE;
        int dr = mouse_row - cat->actor.y / TILE;
        bool far = dc * dc + dr * dr > 36;
        *col = far ? mouse_col : cat->home_col;
        *row = far ? mouse_row : cat->home_row;
    } else {
        *col = mouse_col;
        *row = mouse_row;
    }
}

/* At a tile center: never back the way it came, else the open way closest
   to its target, or a random one when scared. */
static void choose_cat_dir(int index)
{
    cat_t *cat = &cats[index];
    actor_t *actor = &cat->actor;
    int back = opposite(actor->dir);
    int options[4];
    int option_count = 0;
    for (int dir = 0; dir < 4; ++dir) {
        if (dir != back && can_go(actor, dir)) {
            options[option_count++] = dir;
        }
    }
    if (option_count == 0) {
        actor->dir = back;
        return;
    }
    if (cat->mode == CAT_SCARED) {
        actor->dir = options[esp_random() % option_count];
        return;
    }
    int target_col;
    int target_row;
    cat_target(index, &target_col, &target_row);
    int best = options[0];
    int best_distance = INT32_MAX;
    for (int i = 0; i < option_count; ++i) {
        int col = actor->x / TILE + DX[options[i]];
        int row = actor->y / TILE + DY[options[i]];
        int distance = (col - target_col) * (col - target_col) +
                       (row - target_row) * (row - target_row);
        if (distance < best_distance) {
            best = options[i];
            best_distance = distance;
        }
    }
    actor->dir = best;
}

static int cat_speed(const cat_t *cat)
{
    switch (cat->mode) {
    case CAT_SCARED:
        return SCARED_SPEED;
    case CAT_EYES:
        return EYES_SPEED;
    default: {
        int speed = CAT_SPEED + level - 1;
        return speed > CAT_SPEED_MAX ? CAT_SPEED_MAX : speed;
    }
    }
}

static void move_cat(int index)
{
    cat_t *cat = &cats[index];
    actor_t *actor = &cat->actor;
    if (cat->mode == CAT_BASKET) {
        if (--cat->timer <= 0) {
            place(actor, EXIT_COL * TILE, EXIT_ROW * TILE, DIR_LEFT);
            cat->mode = scared_frames > 0 ? CAT_SCARED : CAT_HUNT;
        }
        return;
    }
    int pixels = pixels_this_frame(actor, cat_speed(cat));
    for (int i = 0; i < pixels; ++i) {
        if (at_tile_center(actor)) {
            if (cat->mode == CAT_EYES && actor->x == EXIT_COL * TILE &&
                actor->y == EXIT_ROW * TILE) {
                /* Home: back into the basket for a moment. */
                place(actor, cat->basket_x, BASKET_Y, DIR_LEFT);
                cat->mode = CAT_BASKET;
                cat->timer = FRAMES(1.5);
                return;
            }
            choose_cat_dir(index);
        }
        step(actor);
    }
}

/* Wander for 7 seconds, hunt for 20, and again; cats turn round at each change. */
static void update_cat_mode(void)
{
    ++mode_frames;
    int phase = wandering ? FRAMES(7) : FRAMES(20);
    if (mode_frames < phase) {
        return;
    }
    mode_frames = 0;
    wandering = !wandering;
    for (int i = 0; i < CAT_COUNT; ++i) {
        if (cats[i].mode == CAT_HUNT) {
            cats[i].actor.dir = opposite(cats[i].actor.dir);
        }
    }
}

/* Returns true when a cat caught the mouse. */
static bool check_catches(void)
{
    for (int i = 0; i < CAT_COUNT; ++i) {
        cat_t *cat = &cats[i];
        if (cat->mode == CAT_BASKET || cat->mode == CAT_EYES ||
            abs(cat->actor.x - mouse.x) >= CATCH_DISTANCE ||
            abs(cat->actor.y - mouse.y) >= CATCH_DISTANCE) {
            continue;
        }
        if (cat->mode == CAT_SCARED) {
            /* 200, 400, 800 for each cat caught on one big cheese. */
            score += CAT_POINTS << (cats_caught < 3 ? cats_caught : 3);
            ++cats_caught;
            cat->mode = CAT_EYES;
            cat->actor.sub = 0;
            draw_hud();
        } else {
            return true;
        }
    }
    return false;
}

/* ---- Playing ---- */

static void reset_actors(void)
{
    static const uint16_t colors[CAT_COUNT] = {
        RGB565(255, 140, 30),  /* orange */
        RGB565(235, 235, 235), /* white */
        RGB565(240, 90, 200),  /* pink */
    };
    static const int homes[CAT_COUNT][2] = {{COLS - 1, 0}, {0, 0}, {0, ROWS - 1}};
    /* The basket holds three cats side by side. */
    static const int basket_x[CAT_COUNT] = {8 * TILE + 4, 9 * TILE + 8, 10 * TILE + 12};
    place(&mouse, MOUSE_START_COL * TILE, MOUSE_START_ROW * TILE, DIR_LEFT);
    wanted_dir = DIR_NONE;
    pressed_key = -1;
    mouse_facing = DIR_LEFT;
    for (int i = 0; i < CAT_COUNT; ++i) {
        cat_t *cat = &cats[i];
        cat->color = colors[i];
        cat->home_col = homes[i][0];
        cat->home_row = homes[i][1];
        cat->basket_x = basket_x[i];
        cat->mode = CAT_BASKET;
        place(&cat->actor, basket_x[i], BASKET_Y, DIR_LEFT);
    }
    /* The first cat starts outside; the others follow, sooner each level. */
    int delay = FRAMES(3) - (level - 1) * FRAMES(0.5);
    delay = delay < FRAMES(0.5) ? FRAMES(0.5) : delay;
    cats[0].timer = 1;
    cats[1].timer = delay;
    cats[2].timer = 3 * delay;
    scared_frames = 0;
    mode_frames = 0;
    wandering = true;
    mouse.old_x = mouse.x;
    mouse.old_y = mouse.y;
    for (int i = 0; i < CAT_COUNT; ++i) {
        cats[i].actor.old_x = cats[i].actor.x;
        cats[i].actor.old_y = cats[i].actor.y;
    }
}

static void reset_maze(void)
{
    cheese_left = 0;
    for (int row = 0; row < ROWS; ++row) {
        memcpy(maze[row], MAZE_LAYOUT[row], COLS + 1);
        for (int col = 0; col < COLS; ++col) {
            cheese_left += maze[row][col] == '.' || maze[row][col] == 'o';
        }
    }
}

/* Steers with the WASD pad: a touch on or near it picks the nearest key,
   which lights while held. Touches elsewhere do nothing. */
static void read_touch(void)
{
    cyd_touch_point_t point;
    int key = -1;
    if (cyd_touch_read(&point) && point.x >= PAD_X - PAD_SLOP && point.y >= PAD_Y - PAD_SLOP) {
        int best_distance = INT32_MAX;
        for (int i = 0; i < 4; ++i) {
            int dx = point.x - (PAD_X + PAD_KEYS[i].x + KEY_WIDTH / 2);
            int dy = point.y - (PAD_Y + PAD_KEYS[i].y + KEY_HEIGHT / 2);
            if (dx * dx + dy * dy < best_distance) {
                best_distance = dx * dx + dy * dy;
                key = i;
            }
        }
        wanted_dir = PAD_KEYS[key].dir;
    }
    if (key != pressed_key) {
        pressed_key = key;
        draw_pad();
    }
}

static void redraw_actors(void)
{
    for (int i = 0; i < CAT_COUNT; ++i) {
        redraw_actor(&cats[i].actor);
    }
    redraw_actor(&mouse);
}

static void flash_walls(void)
{
    for (int i = 0; i < 6; ++i) {
        walls_flash = i % 2 == 0;
        draw_maze();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    walls_flash = false;
}

/* A word across the middle of the maze for a moment. */
static void announce(const char *text, uint16_t color, int milliseconds)
{
    text_row(CYD_TEXT_ROWS / 2, color, text);
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
    draw_maze();
}

/* Plays one maze until it is cleared (true) or the last life is lost. */
static bool play_level(void)
{
    char ready[24];
    snprintf(ready, sizeof(ready), "LEVEL %d", level % 100);
    reset_maze();
    reset_actors();
    draw_hud();
    draw_maze();
    announce(ready, CYD_COLOR_YELLOW, 1500);
    TickType_t last_frame = xTaskGetTickCount();
    while (true) {
        ++frame_count;
        read_touch();
        move_mouse();
        for (int i = 0; i < CAT_COUNT; ++i) {
            move_cat(i);
        }
        bool caught = check_catches();
        if (scared_frames > 0 && --scared_frames == 0) {
            for (int i = 0; i < CAT_COUNT; ++i) {
                if (cats[i].mode == CAT_SCARED) {
                    cats[i].mode = CAT_HUNT;
                }
            }
        }
        update_cat_mode();
        redraw_actors();
        if (frame_count % 8 == 0) {
            big_cheese_shown = !big_cheese_shown;
            draw_big_cheeses();
        }

        if (cheese_left == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            flash_walls();
            return true;
        }
        if (caught) {
            --lives;
            ESP_LOGI(TAG, "Caught on level %d; %d lives left", level, lives);
            draw_hud();
            vTaskDelay(pdMS_TO_TICKS(1200));
            if (lives == 0) {
                return false;
            }
            reset_actors();
            draw_maze();
            announce("READY", CYD_COLOR_YELLOW, 1000);
            last_frame = xTaskGetTickCount();
        }
        vTaskDelayUntil(&last_frame, pdMS_TO_TICKS(FRAME_MS));
    }
}

static void game_task(void *argument)
{
    (void)argument;
    load_high_score();
    while (true) {
        show_title();
        score = 0;
        lives = START_LIVES;
        level = 1;
        clear_screen();
        while (play_level()) {
            ++level;
        }
        show_game_over();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_init());
    BaseType_t result = xTaskCreate(game_task, "game", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
