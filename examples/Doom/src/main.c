/* examples/doom/src/main.c — Doom-ish mini shooter for Vaporware SDK.
 *
 * Designed for the Raz DC25000 / N32G031K8Q7-1 target used by Vaporware:
 * 128x160 GC9107 RGB565 display, one PA7 button, no coil use.
 *
 * This is NOT the real Doom engine and it does not load WAD files.  It is a
 * compact fake-3D, colored, Doom-flavored game that fits the tiny MCU.
 */

#include "app.h"
#include "display.h"
#include "button.h"
#include "battery.h"
#include "system.h"
#include "doom_title_letterbox.h"
#include "doom_enemy_sprites.h"
#include "doom_deathscreen.h"
#include <stdint.h>

#define C_SKY      COL_RGB(18,  22,  45)
#define C_SKY2     COL_RGB(38,  38,  70)
#define C_FLOOR    COL_RGB(35,  30,  26)
#define C_FLOOR2   COL_RGB(54,  43,  33)
#define C_WALL_A   COL_RGB(130, 30,  22)
#define C_WALL_B   COL_RGB(93,  19,  16)
#define C_WALL_C   COL_RGB(180, 62,  24)
#define C_METAL    COL_RGB(74,  78,  82)
#define C_PANEL    COL_RGB(58,  32,  30)
#define C_STRIPE   COL_RGB(210, 165, 22)
#define C_DARK     COL_RGB(12,  12,  12)
#define C_HUD      COL_RGB(34,  34,  34)
#define C_HUD2     COL_RGB(78,  68,  52)
#define C_FACE     COL_RGB(210, 160, 110)
#define C_GUN      COL_RGB(92,  92,  92)
#define C_GUN2     COL_RGB(160, 160, 150)
#define C_FIRE     COL_RGB(255, 210, 50)
#define C_BLOOD    COL_RGB(180, 0,   0)
#define C_GREEN    COL_RGB(0,   210, 50)

#define GAME_TITLE 0
#define GAME_RUN   1
#define GAME_WIN   2
#define GAME_DEAD  3

#define GAME_FRAME_DIV 15U
#define TITLE_HOLD_MS 10000U
#define MAG_SIZE 6U
#define RELOAD_TICKS 2U
#define ENEMY_TYPES 7U
#define BOSS_KIND 7U
#define BOSS_KILLS 6U

static uint8_t mode;
static uint8_t redraw_all;
static uint16_t zpos;
static uint32_t game_frame;
static uint16_t rng_state;
static uint8_t health;
static uint8_t ammo;
static uint8_t reload_ticks;
static uint8_t enemy_active;
static uint8_t enemy_kind;
static uint8_t enemy_hp;
static uint8_t enemy_max_hp;
static uint8_t enemy_spawn_ticks;
static uint8_t enemy_attack_ticks;
static uint8_t kills;
static uint8_t boss_pending;
static uint8_t flash_frames;
static uint8_t hurt_frames;
static uint8_t last_hold;

static void rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0 || h == 0) return;
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    display_fill_rect(x, y, w, h, c);
}

static void draw_digit(uint8_t d, uint16_t x, uint16_t y, uint16_t c) {
    static const uint8_t bits[10][5] = {
        {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
        {7,4,7,1,7},{7,4,7,5,7},{7,1,1,2,2},{7,5,7,5,7},{7,5,7,1,7}
    };
    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t col = 0; col < 3; col++) {
            if (bits[d][r] & (1U << (2U - col))) rect(x + col*2, y + r*2, 2, 2, c);
        }
    }
}

static void draw_num(uint16_t n, uint16_t x, uint16_t y, uint16_t c) {
    if (n >= 100) { draw_digit((n / 100) % 10, x, y, c); x += 8; }
    if (n >= 10)  { draw_digit((n / 10) % 10, x, y, c); x += 8; }
    draw_digit(n % 10, x, y, c);
}

static uint8_t rand8(void) {
    rng_state = (uint16_t)(rng_state * 109U + 89U);
    return (uint8_t)(rng_state >> 8);
}

static void schedule_enemy_spawn(void) {
    enemy_spawn_ticks = boss_pending ? 2U : (uint8_t)(4U + (rand8() & 7U)); /* 1-5.5 seconds at 2 FPS */
}

static void spawn_enemy(void) {
    enemy_active = 1;
    if (boss_pending) {
        boss_pending = 0;
        enemy_kind = BOSS_KIND;
        enemy_max_hp = 10;
        enemy_attack_ticks = 2;
    } else {
        enemy_kind = (uint8_t)(rand8() % ENEMY_TYPES);
        enemy_max_hp = (enemy_kind == 3U) ? 2U :
                       (enemy_kind == 4U) ? 5U :
                       (enemy_kind == 5U) ? 4U :
                       (enemy_kind == 6U) ? 6U :
                       (uint8_t)(2U + enemy_kind);
        enemy_attack_ticks = (uint8_t)(3U + (rand8() & 3U));
    }
    enemy_hp = enemy_max_hp;
}

static void draw_title(void) {
    doom_title_letterbox_draw();
    rect(34, 140, 60, 4, C_FIRE);
}

static void draw_light_strip(uint16_t y, uint8_t phase) {
    for (uint8_t i = 0; i < 4; i++) {
        uint16_t x = (uint16_t)(18 + i * 28 + ((phase + i) & 3));
        rect(x, y, 12, 3, COL_RGB(220, 190, 120));
        rect(x + 2, y + 3, 8, 1, COL_RGB(80, 65, 42));
    }
}

static void draw_grit(uint8_t phase) {
    for (uint8_t i = 0; i < 20; i++) {
        uint16_t x = (uint16_t)((i * 19U + phase * 3U) & 127U);
        uint16_t y = (uint16_t)(7U + ((i * 11U + phase) % 44U));
        rect(x, y, (i & 3U) ? 2U : 3U, 1, (i & 1U) ? COL_RGB(33, 34, 43) : COL_RGB(21, 22, 29));
    }
    for (uint8_t i = 0; i < 32; i++) {
        uint16_t x = (uint16_t)((i * 23U + phase * 5U) & 127U);
        uint16_t y = (uint16_t)(82U + ((i * 9U + phase * 2U) % 50U));
        rect(x, y, (i & 1U) ? 2U : 3U, 2, (i & 1U) ? C_FLOOR2 : COL_RGB(24, 21, 18));
    }
}

static void draw_sprite_scaled(const doom_enemy_sprite_t *sprite, int16_t x, int16_t y, uint16_t w, uint16_t h) {
    uint16_t run[128];
    uint16_t src_stride = (uint16_t)((sprite->width + 1U) / 2U);

    if (w == 0 || h == 0) return;

    for (uint16_t dy = 0; dy < h; dy++) {
        int16_t dst_y = (int16_t)(y + (int16_t)dy);
        int16_t run_x = -1;
        uint16_t run_len = 0;
        uint16_t sy;
        const uint8_t *src_row;

        if (dst_y < 0 || dst_y >= LCD_HEIGHT) continue;

        sy = (uint16_t)(((uint32_t)dy * sprite->height) / h);
        src_row = &sprite->pixels_4bpp[sy * src_stride];

        for (uint16_t dx = 0; dx < w; dx++) {
            int16_t dst_x = (int16_t)(x + (int16_t)dx);
            uint16_t sx = (uint16_t)(((uint32_t)dx * sprite->width) / w);
            uint8_t packed = src_row[sx >> 1];
            uint8_t idx = (sx & 1U) ? (packed & 0x0FU) : (packed >> 4);

            if (idx == 0 || dst_x < 0 || dst_x >= LCD_WIDTH) {
                if (run_len) {
                    display_draw_image(run, (uint16_t)run_x, (uint16_t)dst_y, run_len, 1);
                    run_len = 0;
                }
                continue;
            }

            if (run_len == 0) {
                run_x = dst_x;
                run[run_len++] = sprite->palette[idx];
            } else if (dst_x == (int16_t)(run_x + (int16_t)run_len)) {
                run[run_len++] = sprite->palette[idx];
            } else {
                display_draw_image(run, (uint16_t)run_x, (uint16_t)dst_y, run_len, 1);
                run_x = dst_x;
                run_len = 0;
                run[run_len++] = sprite->palette[idx];
            }
        }

        if (run_len) {
            display_draw_image(run, (uint16_t)run_x, (uint16_t)dst_y, run_len, 1);
        }
    }
}

static void draw_enemy_sprite(uint32_t frame) {
    static const uint8_t grow[DOOM_ENEMY_SPRITE_COUNT] = {4U, 5U, 6U, 2U, 5U, 6U, 8U, 10U};
    static const uint8_t grounded[DOOM_ENEMY_SPRITE_COUNT] = {1U, 1U, 1U, 0U, 1U, 0U, 1U, 1U};
    static const uint8_t floor_y[DOOM_ENEMY_SPRITE_COUNT] = {135U, 135U, 135U, 0U, 133U, 0U, 132U, 134U};
    static const uint8_t hover_y[DOOM_ENEMY_SPRITE_COUNT] = {0U, 0U, 0U, 76U, 0U, 86U, 0U, 0U};
    const doom_enemy_sprite_t *sprite;
    uint8_t pulse;
    uint16_t draw_h;
    uint16_t draw_w;
    int16_t draw_x;
    int16_t draw_y;

    if (!enemy_active || enemy_hp == 0 || enemy_kind >= DOOM_ENEMY_SPRITE_COUNT) return;

    sprite = &doom_enemy_sprites[enemy_kind];
    pulse = (uint8_t)((frame + enemy_kind * 3U) & 3U);
    draw_h = (uint16_t)(sprite->height + grow[enemy_kind] + pulse);
    draw_w = (uint16_t)(((uint32_t)sprite->width * draw_h) / sprite->height);
    draw_x = (int16_t)(64 - (int16_t)(draw_w / 2U));

    if (grounded[enemy_kind]) {
        draw_y = (int16_t)(floor_y[enemy_kind] - draw_h);
    } else {
        int16_t bob = (pulse & 1U) ? -1 : 1;
        draw_y = (int16_t)(hover_y[enemy_kind] - (int16_t)(draw_h / 2U) + bob);
    }

    draw_sprite_scaled(sprite, draw_x, draw_y, draw_w, draw_h);

    if (flash_frames) {
        rect((uint16_t)(61U + (pulse & 1U)), (uint16_t)(draw_y + (int16_t)(draw_h / 3U)), 6, 6, COL_WHITE);
    }
}

static void draw_world(uint32_t frame) {
    uint8_t phase = (uint8_t)((zpos >> 4) & 7U);

    /* Doom-ish techbase corridor: cleaner broad shapes, less visual noise. */
    rect(0, 0, 128, 56, COL_RGB(9, 10, 15));
    rect(0, 56, 128, 20, C_WALL_B);
    rect(0, 76, 128, 60, C_FLOOR);
    rect(0, 0, 128, 4, C_DARK);
    rect(18, 5, 92, 3, COL_RGB(22, 22, 30));
    rect(28, 24, 72, 2, COL_RGB(28, 28, 38));
    rect(36, 44, 56, 3, COL_RGB(24, 18, 18));
    draw_light_strip(13, phase);
    draw_light_strip(33, (uint8_t)(phase + 2));
    draw_grit(phase);

    for (uint8_t y = 86; y < 136; y += 12) {
        uint8_t shift = (uint8_t)((zpos >> 3) & 15U);
        rect(0, y, 128, 1, C_FLOOR2);
        rect(16 + shift, y + 5, 96, 1, COL_RGB(25, 22, 20));
        rect(46, y + 2, 3, 6, COL_RGB(68, 56, 42));
        rect(79, y + 2, 3, 6, COL_RGB(68, 56, 42));
    }
    rect(42, 82, 44, 54, COL_RGB(42, 35, 28));
    rect(47, 84, 34, 1, COL_RGB(82, 70, 55));
    rect(50, 98, 28, 1, COL_RGB(82, 70, 55));
    rect(52, 114, 24, 1, COL_RGB(82, 70, 55));
    rect(55, 130, 18, 1, COL_RGB(82, 70, 55));

    /* Broad perspective walls. */
    rect(0, 44, 26, 92, C_PANEL);
    rect(102, 44, 26, 92, C_PANEL);
    rect(26, 54, 8, 72, C_METAL);
    rect(94, 54, 8, 72, C_METAL);
    rect(34, 62, 8, 54, C_WALL_A);
    rect(86, 62, 8, 54, C_WALL_A);
    rect(42, 70, 6, 38, C_METAL);
    rect(80, 70, 6, 38, C_METAL);
    rect(48, 74, 32, 4, C_DARK);
    rect(0, 44, 128, 2, COL_RGB(34, 28, 28));
    rect(0, 134, 128, 2, C_DARK);
    rect(12, 48, 4, 86, COL_RGB(38, 22, 22));
    rect(112, 48, 4, 86, COL_RGB(38, 22, 22));

    /* Panel detail, animated just enough to sell movement at 2 FPS. */
    rect(4, 54, 16, 7, C_WALL_C);
    rect(108, 54, 16, 7, C_WALL_C);
    rect(5, 72, 15, 4, C_STRIPE);
    rect(108, 72, 15, 4, C_STRIPE);
    rect(8, 92 + phase, 10, 18, C_METAL);
    rect(110, 92 + phase, 10, 18, C_METAL);
    rect(35, 83, 9, 3, C_STRIPE);
    rect(84, 83, 9, 3, C_STRIPE);
    rect(52, 57, 24, 9, C_WALL_A);
    rect(55, 60, 18, 3, C_METAL);
    for (uint8_t i = 0; i < 7; i++) {
        uint16_t y = (uint16_t)(49U + i * 12U);
        rect(3, y, 5, 3, (i & 1U) ? C_WALL_B : COL_RGB(95, 39, 31));
        rect(120, y, 5, 3, (i & 1U) ? COL_RGB(95, 39, 31) : C_WALL_B);
        rect(28, (uint16_t)(y + 4U), 3, 3, COL_RGB(116, 120, 120));
        rect(97, (uint16_t)(y + 4U), 3, 3, COL_RGB(116, 120, 120));
    }
    for (uint8_t i = 0; i < 5; i++) {
        uint16_t y = (uint16_t)(60U + i * 14U);
        rect(17, y, 7, 1, COL_RGB(150, 52, 30));
        rect(104, y, 7, 1, COL_RGB(150, 52, 30));
        rect(35, (uint16_t)(y + 7U), 6, 2, COL_RGB(52, 54, 58));
        rect(87, (uint16_t)(y + 7U), 6, 2, COL_RGB(52, 54, 58));
    }

    /* Door/end marker */
    if (zpos > 1320) {
        uint8_t s = (uint8_t)((zpos - 1320) >> 4);
        if (s > 30) s = 30;
        rect(64 - s, 38 - s/2, s*2, 56 + s, C_METAL);
        rect(64 - s + 4, 42 - s/2, (uint16_t)(s*2 - 8), 48 + s, COL_RGB(70, 54, 34));
        rect(61, 59, 6, 20, C_DARK);
        rect(54, 46, 20, 3, C_STRIPE);
    } else {
        rect(54, 48, 20, 18, C_DARK);
        rect(57, 51, 14, 12, COL_RGB(68, 48, 35));
    }

    draw_enemy_sprite(frame);

    /* Muzzle flash */
    if (flash_frames) {
        rect(54, 93, 20, 20, C_FIRE);
        rect(59, 88, 10, 30, COL_WHITE);
    }

    /* Gun and hands, kept above the HUD bar. */
    rect(0, 136, 128, 24, COL_RGB(28, 28, 30));
    rect(0, 138, 128, 2, C_HUD2);
    rect(0, 157, 128, 3, C_DARK);
    for (uint8_t i = 0; i < 8; i++) {
        rect((uint16_t)(i * 16U), 136, 8, 1, (i & 1U) ? C_STRIPE : C_METAL);
        rect((uint16_t)(i * 16U + 10U), 153, 4, 2, COL_RGB(48, 48, 48));
    }
    rect(39, 128, 17, 7, COL_RGB(132, 86, 55));
    rect(75, 128, 17, 7, COL_RGB(132, 86, 55));
    rect(45, 121, 12, 11, C_FACE);
    rect(72, 121, 12, 11, C_FACE);
    rect(50, 116, 30, 16, C_GUN);
    rect(45, 123, 40, 8, COL_RGB(58, 58, 58));
    rect(56, 104, 18, 19, C_GUN2);
    rect(59, 97, 12, 11, COL_RGB(190, 190, 180));
    rect(61, 91, 8, 10, COL_RGB(78, 78, 78));
    rect(53, 111, 24, 4, COL_RGB(42, 42, 42));
    rect(58, 101, 14, 3, COL_RGB(232, 232, 218));
    rect(61, 94, 8, 3, C_DARK);
    rect(48, 131, 36, 4, C_DARK);
    rect(55, 124, 5, 3, COL_RGB(205, 205, 190));
    rect(70, 124, 5, 3, COL_RGB(205, 205, 190));
    rect(61, 116, 8, 4, C_DARK);

    /* HUD */
    rect(1, 141, 32, 16, COL_RGB(18, 22, 18));
    rect(34, 141, 10, 16, COL_RGB(42, 35, 32));
    rect(84, 141, 40, 16, COL_RGB(24, 20, 18));
    rect(86, 143, 36, 1, COL_RGB(84, 72, 48));
    draw_num(health, 4, 144, (health < 30) ? C_BLOOD : C_GREEN);
    if (reload_ticks) {
        rect(90, 144, 28, 10, C_STRIPE);
        rect(93, 147, 22, 2, C_DARK);
    } else {
        draw_num(ammo, 100, 144, C_FIRE);
    }
    rect(47, 141, 34, 16, COL_RGB(92, 62, 44));
    rect(50, 143, 28, 12, C_FACE);
    rect(55, 146, 4, 3, C_DARK); rect(70, 146, 4, 3, C_DARK);
    rect(60, 153, 9, 2, C_BLOOD);
    rect(52, 141, 24, 2, COL_RGB(235, 185, 128));
    if (hurt_frames) rect(0, 0, 128, 8, C_BLOOD);
    (void)frame;
}

static void start_game(void) {
    mode = GAME_RUN;
    zpos = 0;
    game_frame = 0;
    rng_state = (uint16_t)(ms_now() ^ 0xA53CU);
    health = 100;
    ammo = MAG_SIZE;
    reload_ticks = 0;
    enemy_active = 0;
    enemy_kind = 0;
    enemy_hp = 0;
    enemy_max_hp = 0;
    enemy_attack_ticks = 0;
    kills = 0;
    boss_pending = 0;
    schedule_enemy_spawn();
    flash_frames = 0;
    hurt_frames = 0;
    last_hold = 0;
    redraw_all = 1;
}

static void draw_banner_glyph(const uint8_t rows[7], uint16_t x, uint16_t y,
                              uint16_t s, uint16_t c) {
    for (uint8_t row = 0; row < 7; row++) {
        for (uint8_t col = 0; col < 5; col++) {
            if (rows[row] & (1U << (4U - col))) {
                rect((uint16_t)(x + col * s), (uint16_t)(y + row * s), s, s, c);
            }
        }
    }
}

static void draw_victory_banner(void) {
    static const uint8_t glyphs[7][7] = {
        {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04}, /* V */
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}, /* I */
        {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}, /* C */
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
        {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
        {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    };
    const uint16_t shadow = COL_RGB(0, 88, 0);
    const uint16_t hi = COL_RGB(122, 255, 92);
    const uint16_t mid = C_GREEN;
    uint16_t x = 4;
    uint16_t y = 68;
    const uint16_t scale = 3;

    for (uint8_t i = 0; i < 7; i++) {
        draw_banner_glyph(glyphs[i], (uint16_t)(x + 1U), (uint16_t)(y + 2U), scale, shadow);
        draw_banner_glyph(glyphs[i], x, y, scale, mid);
        draw_banner_glyph(glyphs[i], x, y, scale, hi);
        rect(x, y, (uint16_t)(5U * scale), 1, hi);
        x = (uint16_t)(x + 17U);
    }

    rect(12, 91, 104, 2, hi);
    rect(10, 93, 108, 2, mid);
}

static void draw_end(uint8_t win) {
    display_fill(win ? COL_RGB(0, 30, 0) : COL_RGB(40, 0, 0));
    if (win) {
        display_fill(COL_RGB(76, 67, 47));
        rect(0, 18, 128, 126, COL_RGB(58, 58, 58));
        rect(0, 18, 128, 2, COL_RGB(120, 125, 104));
        rect(0, 142, 128, 2, COL_RGB(34, 34, 34));
        rect(0, 20, 4, 122, COL_RGB(190, 190, 170));
        rect(4, 20, 2, 122, COL_RGB(86, 86, 76));
        rect(0, 144, 128, 16, COL_RGB(22, 22, 22));

        for (uint8_t i = 0; i < 7; i++) {
            uint16_t y = (uint16_t)(24U + i * 10U);
            rect(1, y, 3, 7, COL_RGB(132, 132, 124));
            rect(2, (uint16_t)(y + 1U), 1, 5, COL_RGB(92, 92, 88));
        }

        for (uint8_t i = 0; i < 5; i++) {
            uint16_t y = (uint16_t)(38U + i * 24U);
            rect(6, y, 122, 1, COL_RGB(38, 38, 38));
            rect(6, (uint16_t)(y + 1U), 122, 1, COL_RGB(92, 92, 92));
        }

        rect(10, 54, 28, 1, COL_RGB(45, 45, 45));
        rect(90, 54, 28, 1, COL_RGB(45, 45, 45));
        rect(10, 102, 28, 1, COL_RGB(45, 45, 45));
        rect(90, 102, 28, 1, COL_RGB(45, 45, 45));
        draw_victory_banner();

        for (uint8_t i = 0; i < 8; i++) {
            uint16_t x = (uint16_t)(i * 16U);
            rect(x, 144, 8, 16, C_BLOOD);
            rect((uint16_t)(x + 8U), 144, 8, 16, COL_RGB(18, 18, 18));
            rect((uint16_t)(x + 2U), 144, 3, 16, COL_RGB(255, 110, 110));
            rect((uint16_t)(x + 10U), 144, 3, 16, C_DARK);
        }
    } else {
        doom_deathscreen_draw();
        rect(0, 0, 128, 6, COL_RGB(18, 0, 0));
        rect(16, 150, 96, 3, C_FIRE);
    }
}

void app_init(void) {
    app_set_sleep_timeout(30000);
    app_set_hold_reset(12000, 0);
    mode = GAME_TITLE;
    redraw_all = 1;
    draw_title();
}

void app_update(uint32_t frame) {
    uint8_t force_draw = 0;

    if (mode == GAME_TITLE) {
        if ((frame & 15U) == 0) rect(34, 140, 60, 4, (frame & 16U) ? C_FIRE : C_GREEN);
        if (button_just_pressed()) start_game();
        return;
    }

    if (button_pressed() && button_held_ms() >= TITLE_HOLD_MS) {
        mode = GAME_TITLE;
        draw_title();
        return;
    }

    if (mode == GAME_WIN || mode == GAME_DEAD) {
        if (mode == GAME_DEAD && (frame & 15U) == 0U) {
            rect(16, 150, 96, 3, (frame & 16U) ? C_FIRE : C_HUD2);
        }
        if (button_just_pressed()) { mode = GAME_TITLE; draw_title(); }
        return;
    }

    /* GAME_RUN */
    if (button_just_pressed() && ammo > 0 && reload_ticks == 0) {
        ammo--;
        flash_frames = 1;
        force_draw = 1;
        if (enemy_active && enemy_hp > 0) {
            enemy_hp--;
            if (enemy_hp == 0) {
                enemy_active = 0;
                kills++;
                if ((kills % BOSS_KILLS) == 0) boss_pending = 1;
                zpos += (enemy_kind == BOSS_KIND) ? 160U : 90U;
                schedule_enemy_spawn();
            }
        }
        if (ammo == 0) {
            reload_ticks = RELOAD_TICKS;
        }
    }

    if (!redraw_all && !force_draw && (frame % GAME_FRAME_DIV) != 0) return;
    if (redraw_all) redraw_all = 0;

    if (!force_draw && flash_frames) flash_frames--;
    if (!force_draw && hurt_frames) hurt_frames--;

    if (force_draw) {
        draw_world(game_frame);
        return;
    }

    uint8_t holding = button_pressed() ? 1 : 0;
    if (holding && !last_hold) { /* first hold frame already handled as shot */ }
    last_hold = holding;

    if ((game_frame & (holding ? 3U : 1U)) == 0) zpos += holding ? 2 : 5;

    if (reload_ticks) {
        reload_ticks--;
        if (reload_ticks == 0) {
            ammo = MAG_SIZE;
        }
    } else if (ammo == 0) {
        reload_ticks = RELOAD_TICKS;
    }

    if (!enemy_active) {
        if (enemy_spawn_ticks) enemy_spawn_ticks--;
        if (enemy_spawn_ticks == 0) spawn_enemy();
    } else if (enemy_hp > 0) {
        if (enemy_attack_ticks) enemy_attack_ticks--;
        if (enemy_attack_ticks == 0) {
            uint8_t dmg = (enemy_kind == BOSS_KIND) ? 14U :
                          (enemy_kind == 3U) ? 6U :
                          (enemy_kind == 4U) ? 11U :
                          (enemy_kind == 5U) ? 8U :
                          (enemy_kind == 6U) ? 12U :
                          (uint8_t)(5U + enemy_kind * 3U);
            if (health > dmg) health = (uint8_t)(health - dmg); else health = 0;
            hurt_frames = 2;
            enemy_attack_ticks = (enemy_kind == BOSS_KIND) ? 3U : (uint8_t)(3U + (rand8() & 3U));
        }
    }

    if (health == 0) { mode = GAME_DEAD; draw_end(0); return; }
    if (zpos > 1640) { mode = GAME_WIN; draw_end(1); return; }

    draw_world(game_frame);
    game_frame++;
}

void app_wake(void) {
    redraw_all = 1;
    if (mode == GAME_TITLE) draw_title();
    else if (mode == GAME_WIN) draw_end(1);
    else if (mode == GAME_DEAD) draw_end(0);
    else draw_world(0);
}
