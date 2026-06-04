/* delay_hit/src/main.c — 60-second-delayed-hit, plain countdown UI
 *
 * Press the button to start a 60 s countdown.  When it reaches zero, PA5
 * is driven HIGH and stays HIGH until the button is pressed again (or the
 * HIT_MAX_MS safety cap fires).
 *
 * State machine:
 *   IDLE      "60" shown statically, waiting for an arm press
 *   ARMED     countdown 60..0 in big digits; press cancels
 *   FIRING    solid red screen, PA5 held HIGH; press cancels
 *   COOLDOWN  black for 2 s, then back to IDLE
 *
 * Coil control:
 *   PA5 = coil MOSFET gate on this device variant (confirmed by live scan).
 *   Read-modify-write on MODER only — never write the whole register
 *   (kills SWD).  PA5 stays output-LOW at rest; goes HIGH only while
 *   firing.
 *
 * Timing:
 *   ms_now() wraps at 65535 ms.  60000 ms fits but is close to the wrap;
 *   we use (uint16_t) subtraction so the comparison stays correct across
 *   the wrap point.
 */
#include "app.h"
#include "display.h"
#include "button.h"
#include "system.h"
#include "n32g031.h"

#define ARM_DELAY_MS   60000U   /* total countdown                       */
#define HIT_MAX_MS     1500000U   /* hard safety cap on coil-on time       */
#define COOLDOWN_MS     2000U   /* post-hit screen                       */

#define COIL_PORT       GPIOA
#define COIL_PIN            5   /* PA5 — coil MOSFET gate                */

#define SCALE              10   /* countdown digit scale (50×70 px)      */
#define FIRE_SCALE          5   /* firing counter digit scale (25×35 px) */

#define COL_BG          COL_BLACK
#define COL_FG          COL_WHITE
#define COL_FIRE        COL_RED

/* ── 5×7 pixel font for digits, MSB = leftmost pixel of a 5-bit row ── */
static const uint8_t G_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
static const uint8_t G_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
static const uint8_t G_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
static const uint8_t G_3[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E};
static const uint8_t G_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
static const uint8_t G_5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
static const uint8_t G_6[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
static const uint8_t G_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x10};
static const uint8_t G_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
static const uint8_t G_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x11,0x0E};

static const uint8_t *digit(unsigned d) {
    switch (d) {
    case 0: return G_0; case 1: return G_1; case 2: return G_2;
    case 3: return G_3; case 4: return G_4; case 5: return G_5;
    case 6: return G_6; case 7: return G_7; case 8: return G_8;
    case 9: return G_9; default: return G_0;
    }
}

static void draw_digit_s(uint16_t x, uint16_t y, unsigned d,
                         int s, uint16_t fg) {
    const uint8_t *g = digit(d);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                display_fill_rect(x + col * s, y + row * s, s, s, fg);
            }
        }
    }
}

static void draw_digit(uint16_t x, uint16_t y, unsigned d) {
    draw_digit_s(x, y, d, SCALE, COL_FG);
}

/* Two-digit zero-padded countdown, centered.  Caller must have already
 * cleared the digit area (or the screen) to the background colour.    */
static void draw_two_digit(unsigned n) {
    const int pitch = 5 * SCALE + SCALE;        /* char + 1-cell gap   */
    const int total = 2 * pitch - SCALE;        /* no trailing gap     */
    const int x0    = (LCD_WIDTH  - total) / 2;
    const int y0    = (LCD_HEIGHT - 7 * SCALE) / 2;
    draw_digit(x0,         y0, (n / 10) % 10);
    draw_digit(x0 + pitch, y0, n % 10);
}

/* ── Coil ───────────────────────────────────────────────────────────── */

static void coil_set(uint8_t on) {
    uint32_t m = COIL_PORT->MODER;
    COIL_PORT->MODER = (m & ~(3UL << (COIL_PIN * 2))) |
                       (1UL << (COIL_PIN * 2));
    if (on) GPIO_SET(COIL_PORT, COIL_PIN);
    else    GPIO_CLR(COIL_PORT, COIL_PIN);
}

static void coil_release(void) {
    GPIO_CLR(COIL_PORT, COIL_PIN);
}

/* ── State ─────────────────────────────────────────────────────────── */

enum { ST_IDLE, ST_ARMED, ST_FIRING, ST_COOLDOWN };
static uint8_t  g_state;
static uint16_t g_state_start;
static int8_t   g_last_drawn = -1;          /* last digits rendered    */

/* Coil-on time accumulator.  The general-purpose `elapsed` value above
 * is uint16_t (wraps at 65 s).  To support multi-minute safety caps we
 * accumulate millisecond deltas into a 32-bit counter while in FIRING. */
static uint32_t g_fire_ms;
static uint16_t g_fire_last;
static int16_t  g_fire_secs_drawn = -1;     /* last secs shown on screen */

static void clear_digit_area(void) {
    const int pitch = 5 * SCALE + SCALE;
    const int total = 2 * pitch - SCALE;
    const int x0    = (LCD_WIDTH  - total) / 2;
    const int y0    = (LCD_HEIGHT - 7 * SCALE) / 2;
    display_fill_rect(x0, y0, total, 7 * SCALE, COL_BG);
}

/* 4-digit zero-padded seconds counter on the FIRING screen.
 * Erases its own bounding rect before redraw so digits don't smear when
 * fewer pixels light up (e.g. 1 → 0).                                  */
static void redraw_fire_counter(unsigned secs) {
    if ((int)secs == g_fire_secs_drawn) return;
    if (secs > 9999) secs = 9999;

    const int pitch = 5 * FIRE_SCALE + FIRE_SCALE;
    const int total = 4 * pitch - FIRE_SCALE;
    const int x0    = (LCD_WIDTH  - total) / 2;
    const int y0    = (LCD_HEIGHT - 7 * FIRE_SCALE) / 2;
    display_fill_rect(x0, y0, total, 7 * FIRE_SCALE, COL_FIRE);

    draw_digit_s(x0,             y0, (secs / 1000) % 10, FIRE_SCALE, COL_FG);
    draw_digit_s(x0 + pitch,     y0, (secs / 100)  % 10, FIRE_SCALE, COL_FG);
    draw_digit_s(x0 + 2 * pitch, y0, (secs / 10)   % 10, FIRE_SCALE, COL_FG);
    draw_digit_s(x0 + 3 * pitch, y0,  secs         % 10, FIRE_SCALE, COL_FG);

    g_fire_secs_drawn = (int16_t)secs;
}

static void redraw_countdown(unsigned secs) {
    if ((int)secs == g_last_drawn) return;   /* no flicker on no-change */
    clear_digit_area();
    draw_two_digit(secs);
    g_last_drawn = (int8_t)secs;
}

static void enter_state(uint8_t s) {
    g_state = s;
    g_state_start = ms_now();
    g_last_drawn = -1;

    switch (s) {
    case ST_IDLE:
        display_fill(COL_BG);
        draw_two_digit(60);
        g_last_drawn = 60;
        break;
    case ST_ARMED:
        display_fill(COL_BG);
        draw_two_digit(60);
        g_last_drawn = 60;
        break;
    case ST_FIRING:
        coil_set(1);
        display_fill(COL_FIRE);
        g_fire_ms          = 0;
        g_fire_last        = ms_now();
        g_fire_secs_drawn  = -1;          /* force initial "0000" draw */
        redraw_fire_counter(0);
        break;
    case ST_COOLDOWN:
        display_fill(COL_BG);
        break;
    }
}

/* ── Framework callbacks ────────────────────────────────────────────── */

void app_init(void) {
    app_set_sleep_timeout(0);
    app_set_hold_reset(0, 0);
    enter_state(ST_IDLE);
}

void app_update(uint32_t frame) {
    (void)frame;
    uint16_t elapsed = (uint16_t)(ms_now() - g_state_start);

    switch (g_state) {
    case ST_IDLE:
        if (button_just_pressed()) enter_state(ST_ARMED);
        break;

    case ST_ARMED: {
        if (button_just_pressed()) { enter_state(ST_IDLE); break; }
        unsigned remaining_ms = (elapsed >= ARM_DELAY_MS)
                              ? 0u : (unsigned)(ARM_DELAY_MS - elapsed);
        /* Round up so we display "60" briefly at the start and "0" only
         * for the final tick before firing.                            */
        unsigned secs = (remaining_ms + 999u) / 1000u;
        if (secs > 60) secs = 60;
        redraw_countdown(secs);
        if (elapsed >= ARM_DELAY_MS) enter_state(ST_FIRING);
        break;
    }

    case ST_FIRING: {
        uint16_t now = ms_now();
        g_fire_ms += (uint16_t)(now - g_fire_last);
        g_fire_last = now;
        redraw_fire_counter(g_fire_ms / 1000u);
        if (button_just_pressed() || g_fire_ms >= HIT_MAX_MS) {
            coil_release();
            enter_state(ST_COOLDOWN);
        }
        break;
    }

    case ST_COOLDOWN:
        if (elapsed >= COOLDOWN_MS) enter_state(ST_IDLE);
        break;
    }
}

void app_wake(void) { enter_state(ST_IDLE); }
