/* raycaster.c
 *
 * Steps:
 *   [x] Step 1: Scaffold    — solid red screen, button flashes white
 *   [x] Step 2: Fixed-point — Q12 sin/cos table, animated sine wave
 *   [x] Step 3: draw_vline  — ceiling/wall/floor columns, sine bar test
 *   [x] Step 4: DDA one ray — center column only, static distance view
 *   [x] Step 5: Full frame  — all 128 rays, first 3D view (static)
 *   [x] Step 6: Movement    — player position/angle, button controls
 *   [x] Step 7: Polish      — NS/EW shading, distance fog, battery meter
 *
 * Controls (PA7, active-LOW with pull-up):
 *   Tap  (< ~300 ms): step forward 0.25 map units (with sliding collision)
 *   Hold (> ~300 ms): rotate right continuously while held
 *
 * Camera: dir + plane × (cx/127), FOV ≈ 66°, plane ⊥ dir rotates with angle.
 * DDA:    Q12 fixed-point (1.0 = 4096). perp_dist = side_dist − delta_dist.
 * Fog:    bit-shift per R/G/B channel based on distance bracket.
 * Shade:  EW faces bright, NS faces half-bright (Wolfenstein-style).
 */

#include "n32g031.h"
#include "display.h"
#include "system.h"
#include "battery.h"

/* ── Colors ─────────────────────────────────────────────────────────── */
#define C_CEIL    COL_RGB(  0,   0,  50)
#define C_FLOOR   COL_RGB( 40,  40,  40)
#define C_WALL_EW COL_RGB(  0, 180,   0)   /* East/West  face — bright */
#define C_WALL_NS COL_RGB(  0,  90,   0)   /* North/South face — dark  */

/* ── Q12 sine table (256 entries, 1.0 = 4096) ───────────────────────  */
static const int16_t sin_lut[256] = {
        0,   101,   201,   301,   401,   501,   601,   700,   799,   897,   995,  1092,  1189,  1285,  1380,  1474,
     1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,  2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
     2896,  2967,  3035,  3102,  3166,  3229,  3290,  3349,  3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
     3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,  4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
     4096,  4095,  4091,  4085,  4076,  4065,  4052,  4036,  4017,  3996,  3973,  3948,  3920,  3889,  3857,  3822,
     3784,  3745,  3703,  3659,  3612,  3564,  3513,  3461,  3406,  3349,  3290,  3229,  3166,  3102,  3035,  2967,
     2896,  2824,  2751,  2675,  2598,  2520,  2440,  2359,  2276,  2191,  2106,  2019,  1931,  1842,  1751,  1660,
     1567,  1474,  1380,  1285,  1189,  1092,   995,   897,   799,   700,   601,   501,   401,   301,   201,   101,
        0,  -101,  -201,  -301,  -401,  -501,  -601,  -700,  -799,  -897,  -995, -1092, -1189, -1285, -1380, -1474,
    -1567, -1660, -1751, -1842, -1931, -2019, -2106, -2191, -2276, -2359, -2440, -2520, -2598, -2675, -2751, -2824,
    -2896, -2967, -3035, -3102, -3166, -3229, -3290, -3349, -3406, -3461, -3513, -3564, -3612, -3659, -3703, -3745,
    -3784, -3822, -3857, -3889, -3920, -3948, -3973, -3996, -4017, -4036, -4052, -4065, -4076, -4085, -4091, -4095,
    -4096, -4095, -4091, -4085, -4076, -4065, -4052, -4036, -4017, -3996, -3973, -3948, -3920, -3889, -3857, -3822,
    -3784, -3745, -3703, -3659, -3612, -3564, -3513, -3461, -3406, -3349, -3290, -3229, -3166, -3102, -3035, -2967,
    -2896, -2824, -2751, -2675, -2598, -2520, -2440, -2359, -2276, -2191, -2106, -2019, -1931, -1842, -1751, -1660,
    -1567, -1474, -1380, -1285, -1189, -1092,  -995,  -897,  -799,  -700,  -601,  -501,  -401,  -301,  -201,  -101,
};
#define sin_q12(i) sin_lut[(uint8_t)(i)]
#define cos_q12(i) sin_lut[(uint8_t)((i) + 64)]

/* ── Map — 8×8 with interior pillars/walls to explore ───────────────  */
/* Player spawns at (1.5, 1.5), looking right (angle=0).               */
static const uint8_t map[8][8] = {
    {1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,1},
    {1,0,1,0,0,0,0,1},
    {1,0,0,0,1,0,0,1},
    {1,0,1,0,0,0,0,1},
    {1,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1},
};

/* ── draw_vline ──────────────────────────────────────────────────────  */
static void draw_vline(uint8_t x, uint8_t wall_top, uint8_t wall_bot,
                       uint16_t wall_color)
{
    if (wall_top > 0)
        display_fill_rect(x, 0, 1, wall_top, C_CEIL);
    if (wall_bot > wall_top)
        display_fill_rect(x, wall_top, 1, wall_bot - wall_top, wall_color);
    if (wall_bot < 160)
        display_fill_rect(x, wall_bot, 1, 160 - wall_bot, C_FLOOR);
}

/* ── Distance fog ────────────────────────────────────────────────────  */
/* Shifts each RGB565 channel right based on distance bracket.
 * Bands (Q12 units):  <2u full, 2–3u ÷2, 3–5u ÷4, >5u ÷8            */
static uint16_t fog_color(uint16_t color, int32_t dist)
{
    int shift;
    if      (dist >= 20480) shift = 3;   /* > 5 units  */
    else if (dist >= 12288) shift = 2;   /* > 3 units  */
    else if (dist >=  8192) shift = 1;   /* > 2 units  */
    else                    shift = 0;

    uint8_t r = (uint8_t)(((color >> 11) & 0x1Fu) >> shift);
    uint8_t g = (uint8_t)(((color >>  5) & 0x3Fu) >> shift);
    uint8_t b = (uint8_t)(( color        & 0x1Fu) >> shift);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ── Battery indicator ───────────────────────────────────────────────  */
/* 2×14 px bar at top-right corner, color-coded green/yellow/red.      */
static void draw_battery(void)
{
    uint16_t raw = bat_read_raw();
    uint16_t col;
    if      (raw >= BAT_FULL) col = COL_RGB(  0, 200,   0);
    else if (raw >= BAT_WARN) col = COL_RGB(200, 180,   0);
    else if (raw >= BAT_CRIT) col = COL_RGB(200,   0,   0);
    else                      col = COL_RGB( 60,   0,   0);

    display_fill_rect(125, 0, 3, 16, 0x0000u);  /* black border */
    display_fill_rect(126, 1, 1, 14, col);       /* single-pixel bar */
}

/* ── DDA ray cast ────────────────────────────────────────────────────  */
/* Returns perpendicular wall distance in Q12, sets *out_side:
 *   0 = hit an East/West face (X-aligned crossing)
 *   1 = hit a North/South face (Y-aligned crossing)                   */
static int32_t cast_ray(int32_t px_q12, int32_t py_q12,
                        int32_t rdx,    int32_t rdy,
                        int    *out_side)
{
    int map_x = (int)(px_q12 >> 12);
    int map_y = (int)(py_q12 >> 12);

    int step_x = (rdx >= 0) ? 1 : -1;
    int step_y = (rdy >= 0) ? 1 : -1;

    int32_t frac_x = px_q12 - ((int32_t)map_x << 12);
    int32_t frac_y = py_q12 - ((int32_t)map_y << 12);

    int32_t delta_dist_x, side_dist_x;
    int32_t delta_dist_y, side_dist_y;

    if (rdx == 0) {
        delta_dist_x = 0x7FFFFFFF; side_dist_x = 0x7FFFFFFF;
    } else if (rdx > 0) {
        delta_dist_x = (int32_t)4096 * 4096 / rdx;
        side_dist_x  = (int32_t)(4096 - frac_x) * 4096 / rdx;
    } else {
        delta_dist_x = (int32_t)4096 * 4096 / (-rdx);
        side_dist_x  = (int32_t)frac_x * 4096 / (-rdx);
    }

    if (rdy == 0) {
        delta_dist_y = 0x7FFFFFFF; side_dist_y = 0x7FFFFFFF;
    } else if (rdy > 0) {
        delta_dist_y = (int32_t)4096 * 4096 / rdy;
        side_dist_y  = (int32_t)(4096 - frac_y) * 4096 / rdy;
    } else {
        delta_dist_y = (int32_t)4096 * 4096 / (-rdy);
        side_dist_y  = (int32_t)frac_y * 4096 / (-rdy);
    }

    int side = 0;
    for (int i = 0; i < 32; i++) {
        if (side_dist_x < side_dist_y) {
            side_dist_x += delta_dist_x;
            map_x += step_x;
            side = 0;
        } else {
            side_dist_y += delta_dist_y;
            map_y += step_y;
            side = 1;
        }
        if (map_x < 0 || map_x >= 8 || map_y < 0 || map_y >= 8)
            return 0x7FFFFFFF;
        if (map[map_y][map_x] > 0)
            break;
    }

    *out_side = side;
    return (side == 0) ? (side_dist_x - delta_dist_x)
                       : (side_dist_y - delta_dist_y);
}

/* ── main ────────────────────────────────────────────────────────────  */
int main(void)
{
    *(volatile uint32_t *)0x40003000UL = 0xCCCCUL;
    *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;

    {
        volatile uint32_t *rcc  = (volatile uint32_t *)0x40021018UL;
        volatile uint32_t *modr = (volatile uint32_t *)0x40010800UL;
        volatile uint32_t *bsrr = (volatile uint32_t *)0x40010818UL;
        *rcc  |= (1UL << 2);
        (void)*modr;
        *modr &= ~((3UL<<8)|(3UL<<10)|(3UL<<12));
        *modr |=  ((1UL<<8)|(1UL<<10)|(1UL<<12));
        *bsrr  =  (1UL<<20)|(1UL<<21)|(1UL<<22);
    }

    clock_init();
    delay_ms(50);
    display_init();
    display_set_backlight(80);
    tim1_init();
    bat_init();

    /* PA7 = button, active-LOW, internal pull-up */
    GPIOA->MODER &= ~(3UL << (7u * 2u));
    GPIOA->PUPDR &= ~(3UL << (7u * 2u));
    GPIOA->PUPDR |=  (1UL << (7u * 2u));

    /* ── Player state ──────────────────────────────────────────────── */
    int32_t px    = (int32_t)(1 * 4096 + 2048);  /* 1.5 Q12 */
    int32_t py    = (int32_t)(1 * 4096 + 2048);  /* 1.5 Q12 */
    uint8_t angle = 0;                             /* 0 = right */

    /* ── Button state ──────────────────────────────────────────────── */
    uint8_t btn_prev  = 1u;   /* 1 = released */
    uint8_t btn_held  = 0u;   /* frames held  */
    uint8_t frame_ctr = 0u;   /* battery refresh counter */

/* Tap  < ROT_THRESH frames → step; hold ≥ ROT_THRESH → rotate.
 * At ~100 ms/frame, ROT_THRESH=3 ≈ 300 ms threshold.                  */
#define MOVE_SPEED  1024   /* Q12: 0.25 map units per tap   */
#define ROT_SPEED      4   /* angle-table units per frame   */
#define ROT_THRESH     3   /* frames before hold = rotate   */

    draw_battery();

    while (1) {
        IWDG_FEED();

        /* ── Button ────────────────────────────────────────────────── */
        uint8_t btn           = (uint8_t)((GPIOA->IDR >> 7u) & 1u);
        uint8_t just_released = (btn == 1u && btn_prev == 0u);

        if (btn == 0u) {
            if (btn_held < 255u) btn_held++;
            if (btn_held > ROT_THRESH)
                angle = (uint8_t)(angle + ROT_SPEED);
        }

        if (just_released) {
            if (btn_held <= ROT_THRESH) {
                /* Short tap: step forward with per-axis sliding collision */
                int32_t dx = (int32_t)cos_q12(angle) * MOVE_SPEED >> 12;
                int32_t dy = (int32_t)sin_q12(angle) * MOVE_SPEED >> 12;
                int32_t nx = px + dx;
                int32_t ny = py + dy;
                if (map[(int)(py >> 12)][(int)(nx >> 12)] == 0) px = nx;
                if (map[(int)(ny >> 12)][(int)(px >> 12)] == 0) py = ny;
            }
            btn_held = 0u;
        }
        btn_prev = btn;

        /* ── Camera vectors (rotate with angle) ────────────────────── */
        /* dir = (cos, sin); plane ⊥ dir = (-sin, cos) × (2700/4096)  */
        int32_t dir_x   =  (int32_t)cos_q12(angle);
        int32_t dir_y   =  (int32_t)sin_q12(angle);
        int32_t plane_x = (-(int32_t)sin_q12(angle) * 2700) >> 12;
        int32_t plane_y = (  (int32_t)cos_q12(angle) * 2700) >> 12;

        /* ── Render 128 columns ─────────────────────────────────────── */
        for (uint8_t x = 0; x < 128u; x++) {
            IWDG_FEED();

            int32_t cx  = (int32_t)(2u * x) - 127;
            int32_t rdx = dir_x + plane_x * cx / 127;
            int32_t rdy = dir_y + plane_y * cx / 127;

            int     side = 0;
            int32_t dist = cast_ray(px, py, rdx, rdy, &side);

            int32_t wall_h = (dist > 0 && dist < 0x7FFFFFFF)
                           ? (160 * 4096 / dist) : 0;
            if (wall_h > 160) wall_h = 160;

            uint8_t wh       = (uint8_t)wall_h;
            uint8_t wall_top = (uint8_t)((160u - wh) / 2u);
            uint8_t wall_bot = (uint8_t)(wall_top + wh);

            /* EW = bright, NS = dark; then fog both */
            uint16_t wc = (side == 0) ? C_WALL_EW : C_WALL_NS;
            wc = fog_color(wc, dist);

            draw_vline(x, wall_top, wall_bot, wc);
        }

        /* Battery refresh every 256 frames (~25 s at 10fps) */
        if (++frame_ctr == 0u)
            draw_battery();
    }
}
