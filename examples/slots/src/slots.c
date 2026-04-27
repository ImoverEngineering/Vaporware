/* slots.c — Vaporware Slot Machine
 *
 * 128×160 GC9107 display, same hardware as tamagotchi.
 *
 * GAMEPLAY
 *   Press the button to spin 3 reels.
 *   Reels stop left → right with a satisfying click.
 *   Matching symbols on the centre payline determines payout.
 *
 *   ☁ ☁ ☁  JACKPOT
 *   7  7  7  BIG WIN
 *   ♦  ♦  ♦  BIG WIN
 *   X  X  X  WIN
 *   X  X  ?  NEAR MISS
 *   any ☁   NEAR MISS
 *   else     LOSE
 *
 * DISPLAY LAYOUT (128 × 160)
 *   Y=0..19   Top bar  — title + spin count
 *   Y=20..23  Border
 *   Y=24..95  Reel area — 3 rows × 24 px symbols
 *   Y=96..99  Border
 *   Y=100..143 Result area — win/lose label + combo
 *   Y=144..159 Bottom bar — instructions
 *
 * REEL LAYOUT  (3 × 40 px wide, 4 px gaps = 128 px total)
 *   Reel 0: x=0..39   Reel 1: x=44..83   Reel 2: x=88..127
 *   Payline = middle row: Y=48..71
 */
#include "slots.h"
#include "display.h"
#include "nv.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* =====================================================================
 * Layout constants
 * ===================================================================== */
#define TOP_BAR_H    20
#define BORDER_H      4
#define REEL_Y       24        /* top of reel window          */
#define REEL_H       72        /* 3 × SYMBOL_H                */
#define SYMBOL_H     24        /* one symbol row height        */
#define REEL_W       40        /* width of each reel box       */
#define REEL_GAP      4        /* gap between reels            */
#define N_REELS       3
#define PAYLINE_TOP  (REEL_Y + SYMBOL_H)     /* 48 */
#define PAYLINE_BOT  (REEL_Y + SYMBOL_H * 2) /* 72 */
#define RESULT_Y    100
#define BOT_BAR_Y   144

static const uint8_t REEL_X[N_REELS] = { 0, 44, 88 };
/* 0+40=40 +4=44  44+40=84 +4=88  88+40=128  ✓ */

/* =====================================================================
 * Symbols
 * ===================================================================== */
#define N_SYM  8
/* 0=CHERRY 1=LEMON 2=ORANGE 3=BELL 4=STAR 5=SEVEN 6=DIAMOND 7=CLOUD */

/* Weights for random symbol selection — total must be 32 (power of 2 helps) */
static const uint8_t SYM_WEIGHT[N_SYM] = { 7, 6, 5, 5, 4, 3, 1, 1 };
/* CHERRY=7, LEMON=6, ORANGE=5, BELL=5, STAR=4, SEVEN=3, DIAMOND=1, CLOUD=1 */
/* total=32 */

static const char * const SYM_NAME[N_SYM] = {
    "CHERRY", "LEMON", "ORANGE", "BELL", "STAR", "SEVEN", "DIAMOND", "CLOUD"
};

/* =====================================================================
 * Palette
 * ===================================================================== */
#define C_BG         COL_RGB( 10,   8,  25)
#define C_REEL_BG    COL_RGB( 20,  16,  45)
#define C_BORDER     COL_RGB( 60,  50, 110)
#define C_PAYLINE    COL_RGB(255, 215,   0)  /* gold */
#define C_PAYLINE_HI COL_RGB(255, 240, 120)
#define C_TEXT       COL_RGB(210, 210, 255)
#define C_DIM        COL_RGB( 90,  85, 130)
#define C_WIN_COL    COL_RGB( 50, 255,  80)
#define C_LOSE_COL   COL_RGB(200,  55,  55)
#define C_NEAR_COL   COL_RGB(255, 170,  30)
#define C_JACKPOT    COL_RGB(255, 215,   0)
#define C_TOP_BAR    COL_RGB( 22,  18,  55)
#define C_BOT_BAR    COL_RGB( 18,  14,  45)

/* Symbol accent colours */
#define C_CHERRY   COL_RGB(220,  25,  60)
#define C_STALK    COL_RGB( 50, 180,  50)
#define C_LEMON    COL_RGB(255, 240,  30)
#define C_ORANGE   COL_RGB(255, 145,   0)
#define C_BELL     COL_RGB(255, 210,   0)
#define C_STAR     COL_RGB(255, 220,  50)
#define C_SEVEN    COL_RGB(255,  30,  30)
#define C_DIAMOND  COL_RGB( 80, 220, 255)
#define C_CLOUD    COL_RGB(225, 235, 255)
#define C_SMOKE    COL_RGB(170, 180, 210)

/* =====================================================================
 * State machine
 * ===================================================================== */
typedef enum {
    ST_IDLE,      /* waiting for button press          */
    ST_SPINNING,  /* all reels spinning fast (60 f)    */
    ST_STOP_0,    /* reel 0 snapping into place (20 f) */
    ST_STOP_1,    /* reel 1 snapping (20 f)            */
    ST_STOP_2,    /* reel 2 snapping (20 f)            */
    ST_RESULT,    /* show win/lose (90 f)              */
} SlotState;

typedef struct {
    uint8_t  sym;      /* currently displayed middle symbol     */
    uint8_t  target;   /* final payline symbol                  */
    uint8_t  stopped;  /* 1 = locked onto target                */
} Reel;

static SlotState g_state;
static uint32_t  g_timer;        /* frames since state entered  */
static Reel      g_reels[N_REELS];
static uint8_t   g_last_btn;
static uint8_t   g_win_type;     /* 0=lose 1=near 2=win 3=big 4=jackpot */
static uint32_t  g_spins;        /* total spins counter         */
static uint32_t  g_wins;         /* total wins (any kind)       */
static uint8_t   g_dirty;        /* force full redraw on state change */
static uint8_t   g_prev_sym[N_REELS]; /* last drawn mid-symbol per reel */
static uint8_t   g_result_dirty; /* result area needs redraw */

/* LFSR seed: non-zero initial value required (LFSR locks at 0).
 * The seed is NOT re-randomised between game sessions — the sequence is
 * deterministic but appears random to players because no one knows the
 * current LFSR state.  This is adequate for a slot machine game (not
 * cryptographic).  If true unpredictability is needed, mix in ms_now()
 * at first spin (as FlappyVape does with its LCG seed). */
static uint16_t g_lfsr = 0xA5C3;

/* =====================================================================
 * PRNG — 16-bit Galois LFSR (Linear Feedback Shift Register)
 *
 * Produces a pseudo-random sequence with period 2^16 - 1 = 65535 values.
 * Not cryptographically secure, but adequate for a slot machine game.
 * The XOR-shift taps (7, 9, 8) were chosen empirically to avoid short
 * cycles and produce visually acceptable randomness in the symbol stream.
 *
 * The LFSR seed (g_lfsr) is set at compile time (0xA5C3).  It is NOT
 * re-seeded at runtime in this implementation.  All spins in a session
 * follow the same deterministic sequence from wherever the LFSR currently
 * is — indistinguishable from random to a player who cannot observe state.
 * ===================================================================== */
static uint8_t rand8(void) {
    g_lfsr ^= (uint16_t)(g_lfsr << 7);
    g_lfsr ^= (uint16_t)(g_lfsr >> 9);
    g_lfsr ^= (uint16_t)(g_lfsr << 8);
    return (uint8_t)g_lfsr;
}

static uint8_t rand_sym(void) {
    /* Weighted pick: total weight=32, use & 31 for speed */
    uint8_t r = rand8() & 31u;
    uint8_t acc = 0;
    for (uint8_t i = 0; i < N_SYM; i++) {
        acc += SYM_WEIGHT[i];
        if (r < acc) return i;
    }
    return N_SYM - 1;
}

/* =====================================================================
 * Win detection
 *
 * Win table (checked in priority order, highest first):
 *   JACKPOT (type 4) — three CLOUD (sym 7): rarest combo (weight 1 each).
 *                       Expected frequency: (1/32)^3 ≈ 1 in 32768 spins.
 *   BIG WIN (type 3) — three of any symbol with index >= 5 (SEVEN=5, DIAMOND=6).
 *                       SEVEN has weight 3, DIAMOND weight 1 — both are scarce.
 *   WIN     (type 2) — three of any other identical symbol.
 *   LOSE    (type 0) — no matching triple on the payline.
 *
 * "Near miss" (two matching + one different) is NOT detected here;
 * it was removed to simplify the result display.  The win rate is fully
 * determined by the SYM_WEIGHT table in rand_sym().
 * ===================================================================== */
static uint8_t check_win(uint8_t a, uint8_t b, uint8_t c) {
    if (a == 7 && b == 7 && c == 7) return 4; /* JACKPOT — 3× CLOUD (rarest) */
    if (a == b && b == c && a >= 5)  return 3; /* BIG WIN — 3× SEVEN or DIAMOND */
    if (a == b && b == c)            return 2; /* WIN     — 3× any other symbol */
    return 0;                                  /* LOSE    — no payline match */
}

/* =====================================================================
 * Font (5×7 bitmap, column-major LSB-first)
 * ===================================================================== */
static const uint8_t FONT_SP[][5] = {           /* ' ' to '9'    */
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
};
static const uint8_t FONT_AZ[][5] = {           /* 'A' to 'Z'    */
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

static void draw_char(int x, int y, char c, uint16_t col) {
    const uint8_t *g = NULL;
    if (c >= ' ' && c <= '9') g = FONT_SP[(uint8_t)(c - ' ')];
    else if (c >= 'A' && c <= 'Z') g = FONT_AZ[(uint8_t)(c - 'A')];
    else if (c >= 'a' && c <= 'z') g = FONT_AZ[(uint8_t)(c - 'a')];
    if (!g) return;
    for (int i = 0; i < 5; i++) {
        uint8_t b = g[i];
        for (int r = 0; r < 7; r++)
            if (b & (uint8_t)(1u << r))
                display_draw_pixel((uint16_t)(x + i), (uint16_t)(y + r), col);
    }
}

static void draw_str(int x, int y, const char *s, uint16_t col) {
    while (*s) { draw_char(x, y, *s++, col); x += 6; }
}

/* Right-aligned draw_str */
static void draw_str_r(int right_x, int y, const char *s, uint16_t col) {
    int len = 0; const char *p = s; while (*p++) len++;
    draw_str(right_x - len * 6, y, s, col);
}

/* Tiny uint32 → decimal string, right-aligned, width w */
static void draw_num(int x, int y, uint32_t n, uint16_t col) {
    char buf[12];
    uint8_t i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { uint32_t tmp = n; while (tmp) { buf[i++]=(char)('0'+tmp%10); tmp/=10; } }
    /* reverse */
    for (uint8_t l=0, r=i-1; l<r; l++,r--) { char t=buf[l]; buf[l]=buf[r]; buf[r]=t; }
    buf[i] = '\0';
    draw_str(x, y, buf, col);
}

/* =====================================================================
 * Drawing helpers (with Y clip)
 * ===================================================================== */
static void cfr(int x, int y, int w, int h, uint16_t col, int cy0, int cy1) {
    int y0 = (y < cy0) ? cy0 : y;
    int y1 = (y + h > cy1) ? cy1 : (y + h);
    if (y1 <= y0 || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > 128) w = 128 - x;
    if (w <= 0) return;
    display_fill_rect((uint16_t)x, (uint16_t)y0, (uint16_t)w, (uint16_t)(y1 - y0), col);
}

/* Ellipse using horizontal scanlines */
static void cell_ellipse(int cx, int cy, int rx, int ry, uint16_t col,
                         int cy0, int cy1) {
    for (int dy = -ry; dy <= ry; dy++) {
        int num = rx * rx * (ry * ry - dy * dy);
        if (num < 0) continue;
        int hw = rx;
        while (hw * hw * ry * ry > num) hw--;
        cfr(cx - hw, cy + dy, 2 * hw + 1, 1, col, cy0, cy1);
    }
}

/* =====================================================================
 * Symbol drawing
 * Each function draws within a REEL_W × SYMBOL_H cell
 * x,y = top-left of cell; cy0,cy1 = vertical clip range
 * ===================================================================== */

/* 0. CHERRY — two red cherries on a green Y-stem */
static void sym_cherry(int x, int y, int cy0, int cy1) {
    int cx = x + 20;
    /* green Y-stem */
    cfr(cx - 1, y + 2, 2, 7, C_STALK, cy0, cy1);
    cfr(cx - 6, y + 2, 2, 5, C_STALK, cy0, cy1);
    cfr(cx + 4, y + 2, 2, 5, C_STALK, cy0, cy1);
    cfr(cx - 5, y + 6, 2, 2, C_STALK, cy0, cy1); /* left branch join */
    cfr(cx + 3, y + 6, 2, 2, C_STALK, cy0, cy1); /* right branch join */
    /* left cherry */
    cell_ellipse(cx - 8, y + 15, 6, 5, C_CHERRY, cy0, cy1);
    cell_ellipse(cx - 9, y + 14, 2, 2, COL_RGB(255, 100, 100), cy0, cy1); /* highlight */
    /* right cherry */
    cell_ellipse(cx + 7, y + 15, 6, 5, C_CHERRY, cy0, cy1);
    cell_ellipse(cx + 6, y + 14, 2, 2, COL_RGB(255, 100, 100), cy0, cy1);
}

/* 1. LEMON — yellow pointed oval */
static void sym_lemon(int x, int y, int cy0, int cy1) {
    int cx = x + 20, cy = y + 12;
    cell_ellipse(cx, cy, 14, 8, C_LEMON, cy0, cy1);
    /* pointed tips */
    cfr(x + 3, cy - 1, 2, 3, C_LEMON, cy0, cy1);
    cfr(x + 34, cy - 1, 2, 3, C_LEMON, cy0, cy1);
    /* highlight */
    cell_ellipse(cx - 4, cy - 3, 3, 2, COL_RGB(255, 255, 160), cy0, cy1);
    /* tiny green nub at tip */
    cfr(cx - 1, y + 2, 2, 2, C_STALK, cy0, cy1);
}

/* 2. ORANGE — orange circle with green leaf */
static void sym_orange(int x, int y, int cy0, int cy1) {
    int cx = x + 20, cy = y + 13;
    cell_ellipse(cx, cy, 9, 8, C_ORANGE, cy0, cy1);
    /* highlight */
    cell_ellipse(cx - 3, cy - 3, 3, 2, COL_RGB(255, 200, 130), cy0, cy1);
    /* green top + leaf */
    cfr(cx - 1, y + 2, 2, 3, C_STALK, cy0, cy1);
    cfr(cx,     y + 3, 5, 2, C_STALK, cy0, cy1); /* leaf */
}

/* 3. BELL — gold bell with clapper */
static void sym_bell(int x, int y, int cy0, int cy1) {
    int cx = x + 20;
    /* dome (top half of bell) */
    cell_ellipse(cx, y + 9, 9, 8, C_BELL, cy0, cy1);
    /* body: trapezoid flare */
    for (int dy = 0; dy <= 8; dy++) {
        int w = 12 + dy;
        cfr(cx - w / 2, y + 10 + dy, w, 1, C_BELL, cy0, cy1);
    }
    /* rim */
    cfr(cx - 11, y + 18, 22, 2, COL_RGB(200, 160, 0), cy0, cy1);
    /* clapper */
    cell_ellipse(cx, y + 21, 2, 2, COL_RGB(200, 160, 0), cy0, cy1);
    /* highlight */
    cell_ellipse(cx - 4, y + 5, 2, 2, COL_RGB(255, 240, 160), cy0, cy1);
    /* top ring */
    cfr(cx - 2, y + 1, 4, 3, C_BELL, cy0, cy1);
}

/* 4. STAR — 8-point asterisk / starburst */
static void sym_star(int x, int y, int cy0, int cy1) {
    int cx = x + 20, cy = y + 11;
    /* horizontal bar */
    cfr(x + 3, cy - 1, 34, 3, C_STAR, cy0, cy1);
    /* vertical bar */
    cfr(cx - 1, y + 2, 3, 18, C_STAR, cy0, cy1);
    /* diagonals NW-SE and NE-SW: 3px-wide segments */
    for (int i = -7; i <= 7; i++) {
        cfr(cx + i - 1, cy + i - 1, 3, 3, C_STAR, cy0, cy1);
        cfr(cx - i - 1, cy + i - 1, 3, 3, C_STAR, cy0, cy1);
    }
    /* bright centre */
    cell_ellipse(cx, cy, 3, 3, COL_RGB(255, 255, 200), cy0, cy1);
}

/* 5. SEVEN — big bold red "7" */
static void sym_seven(int x, int y, int cy0, int cy1) {
    /* Top horizontal bar */
    cfr(x + 4, y + 2, 30, 5, C_SEVEN, cy0, cy1);
    /* Top-right vertical descent */
    cfr(x + 28, y + 6, 6, 6, C_SEVEN, cy0, cy1);
    /* Diagonal body: steps from top-right down-left */
    for (int i = 0; i < 10; i++) {
        cfr(x + 25 - i * 2, y + 10 + i, 8, 2, C_SEVEN, cy0, cy1);
    }
    /* Base serif */
    cfr(x + 4, y + 18, 14, 4, C_SEVEN, cy0, cy1);
    /* Highlight on top bar */
    cfr(x + 5, y + 2, 28, 2, COL_RGB(255, 130, 130), cy0, cy1);
}

/* 6. DIAMOND — cyan rhombus */
static void sym_diamond(int x, int y, int cy0, int cy1) {
    int cx = x + 20, cy = y + 12;
    /* Draw as horizontal scanlines of a rhombus */
    for (int dy = -10; dy <= 10; dy++) {
        int w = (10 - (dy < 0 ? -dy : dy)) * 14 / 10;
        if (w <= 0) continue;
        cfr(cx - w, cy + dy, 2 * w, 1, C_DIAMOND, cy0, cy1);
    }
    /* Inner highlight (lighter) */
    for (int dy = -4; dy <= -1; dy++) {
        int w = (6 + dy) * 5 / 6;
        if (w <= 0) continue;
        cfr(cx - w - 2, cy + dy, w, 1, COL_RGB(180, 240, 255), cy0, cy1);
    }
}

/* 7. CLOUD — white puffy vape cloud (JACKPOT) */
static void sym_cloud(int x, int y, int cy0, int cy1) {
    /* Multiple overlapping ellipses for cloud body */
    cell_ellipse(x + 10, y + 14, 7, 6, C_CLOUD, cy0, cy1);
    cell_ellipse(x + 18, y + 10, 9, 7, C_CLOUD, cy0, cy1);
    cell_ellipse(x + 28, y + 14, 7, 6, C_CLOUD, cy0, cy1);
    cell_ellipse(x + 22, y + 16, 8, 5, C_CLOUD, cy0, cy1);
    cell_ellipse(x + 14, y + 16, 8, 5, C_CLOUD, cy0, cy1);
    /* Flat bottom fill */
    cfr(x + 3, y + 16, 33, 5, C_CLOUD, cy0, cy1);
    /* Smoke wisps above */
    cfr(x + 14, y + 2, 2, 4, C_SMOKE, cy0, cy1);
    cfr(x + 20, y + 1, 2, 5, C_SMOKE, cy0, cy1);
    cfr(x + 26, y + 3, 2, 3, C_SMOKE, cy0, cy1);
    /* Blue-white highlight on cloud */
    cell_ellipse(x + 16, y + 10, 4, 3, COL_RGB(255, 255, 255), cy0, cy1);
}

/* Dispatcher */
static void draw_symbol(uint8_t sym, int x, int y, int cy0, int cy1) {
    switch (sym & 7u) {
        case 0: sym_cherry (x, y, cy0, cy1); break;
        case 1: sym_lemon  (x, y, cy0, cy1); break;
        case 2: sym_orange (x, y, cy0, cy1); break;
        case 3: sym_bell   (x, y, cy0, cy1); break;
        case 4: sym_star   (x, y, cy0, cy1); break;
        case 5: sym_seven  (x, y, cy0, cy1); break;
        case 6: sym_diamond(x, y, cy0, cy1); break;
        case 7: sym_cloud  (x, y, cy0, cy1); break;
    }
}

/* =====================================================================
 * Reel display helpers
 * ===================================================================== */

/* Which symbol shows in the middle row for reel r at frame f.
 * When stopped, returns the locked target symbol.
 * When spinning, cycles through all N_SYM=8 symbols at a reel-specific rate.
 *
 * RATE[r] = frames per symbol step (higher number = slower spin):
 *   Reel 0: RATE=2 → changes every 2 frames ≈ 15 symbols/sec  (fast)
 *   Reel 1: RATE=3 → changes every 3 frames ≈ 10 symbols/sec  (medium)
 *   Reel 2: RATE=5 → changes every 5 frames ≈  6 symbols/sec  (slow)
 * Using different primes ensures the three reels cycle independently and
 * never appear to synchronise visually during the spin animation.
 */
static uint8_t reel_mid_sym(uint8_t r, uint32_t frame) {
    if (g_reels[r].stopped) return g_reels[r].target;
    static const uint8_t RATE[N_REELS] = { 2, 3, 5 };  /* frames per symbol step */
    return (uint8_t)((frame / RATE[r]) % N_SYM);
}

/* Draw a single reel column (all 3 visible rows) */
static void draw_reel(uint8_t r, uint32_t frame) {
    int rx = (int)REEL_X[r];

    /* Reel background */
    display_fill_rect((uint16_t)rx, REEL_Y, REEL_W, REEL_H, C_REEL_BG);

    /* Left / right border lines */
    display_fill_rect((uint16_t)rx, REEL_Y, 1, REEL_H, C_BORDER);
    display_fill_rect((uint16_t)(rx + REEL_W - 1), REEL_Y, 1, REEL_H, C_BORDER);

    /* Get middle symbol */
    uint8_t mid = reel_mid_sym(r, frame);
    uint8_t top = (uint8_t)((mid + N_SYM - 1) % N_SYM);
    uint8_t bot = (uint8_t)((mid + 1) % N_SYM);

    /* Draw 3 rows: top (dim), mid (payline), bot (dim) */
    int top_y = (int)REEL_Y;
    int mid_y = (int)(REEL_Y + SYMBOL_H);
    int bot_y = (int)(REEL_Y + SYMBOL_H * 2);

    draw_symbol(top, rx, top_y, (int)REEL_Y,               mid_y);
    draw_symbol(mid, rx, mid_y, mid_y,                      bot_y);
    draw_symbol(bot, rx, bot_y, bot_y, (int)(REEL_Y + REEL_H));
}

/* Payline highlight strips (drawn on top of reels) */
static void draw_payline(void) {
    /* Top edge of payline */
    display_fill_rect(0, PAYLINE_TOP, 128, 2, C_PAYLINE);
    /* Bottom edge */
    display_fill_rect(0, PAYLINE_BOT - 2, 128, 2, C_PAYLINE);
    /* Reel gap fills */
    display_fill_rect(40, PAYLINE_TOP, 4, SYMBOL_H, C_PAYLINE);
    display_fill_rect(84, PAYLINE_TOP, 4, SYMBOL_H, C_PAYLINE);
}

/* =====================================================================
 * HUD
 * ===================================================================== */
static void draw_top_bar(void) {
    display_fill_rect(0, 0, 128, TOP_BAR_H, C_TOP_BAR);
    draw_str(4, 6, "VAPORSLOTS", C_PAYLINE);
    /* spin counter right side */
    draw_str(80, 6, "SPINS:", C_DIM);
    draw_num(116, 6, g_spins, C_TEXT);
}

static void draw_bot_bar(void) {
    display_fill_rect(0, BOT_BAR_Y, 128, 160 - BOT_BAR_Y, C_BOT_BAR);
    const char *msg;
    switch (g_state) {
        case ST_IDLE:     msg = "PRESS TO SPIN";  break;
        case ST_SPINNING: msg = "SPINNING...";    break;
        case ST_STOP_0:
        case ST_STOP_1:
        case ST_STOP_2:   msg = "CLICKING...";    break;
        default:          msg = "PRESS TO SPIN";  break;
    }
    /* Center the message */
    int len = 0; const char *p = msg; while(*p++) len++;
    draw_str((128 - len * 6) / 2, BOT_BAR_Y + 5, msg, C_TEXT);
}

static void draw_result_area(void) {
    display_fill_rect(0, RESULT_Y, 128, BOT_BAR_Y - RESULT_Y, C_BG);

    if (g_state != ST_RESULT) return;

    const char *label;
    uint16_t    lcol;
    switch (g_win_type) {
        case 4: label = "JACKPOT!!!";  lcol = C_JACKPOT;  break;
        case 3: label = "BIG WIN!";    lcol = C_WIN_COL;  break;
        case 2: label = "WIN!";        lcol = C_WIN_COL;  break;
        default:label = "NO WIN";      lcol = C_LOSE_COL; break;
    }
    int len = 0; const char *p = label; while(*p++) len++;
    draw_str((128 - len * 6) / 2, RESULT_Y + 4, label, lcol);

    /* Combo label */
    uint8_t a = g_reels[0].target, b = g_reels[1].target, c = g_reels[2].target;
    if (g_win_type >= 2) {
        /* Show winning symbol name */
        draw_str(4, RESULT_Y + 18, SYM_NAME[a], C_TEXT);
    } else {
        /* Show all 3 result names small */
        draw_str( 4, RESULT_Y + 18, SYM_NAME[a], C_DIM);
        draw_str(48, RESULT_Y + 18, SYM_NAME[b], C_DIM);
        draw_str(92, RESULT_Y + 18, SYM_NAME[c], C_DIM);
    }

    /* Win/lose count line */
    draw_str(4, RESULT_Y + 30, "W:", C_DIM);
    draw_num(16, RESULT_Y + 30, g_wins, C_TEXT);
    draw_str_r(124, RESULT_Y + 30,
               (g_win_type > 0) ? "VAPE!" : "NO VAPE", C_DIM);
}

/* =====================================================================
 * Full redraw (called on state change)
 * ===================================================================== */
static void full_redraw(uint32_t frame) {
    /* No full-screen fill — draw every region explicitly to avoid black flash.
     * All pixels are covered by: top bar, borders, reels, reel gaps, result, bot bar. */
    draw_top_bar();
    display_fill_rect(0,  TOP_BAR_H,        128, BORDER_H, C_BORDER); /* top border    */
    display_fill_rect(0,  RESULT_Y - BORDER_H, 128, BORDER_H, C_BORDER); /* mid border */
    /* Reel gap columns (4 px wide, full reel height) */
    display_fill_rect(40, REEL_Y, REEL_GAP, REEL_H, C_BG);
    display_fill_rect(84, REEL_Y, REEL_GAP, REEL_H, C_BG);
    for (uint8_t r = 0; r < N_REELS; r++) draw_reel(r, frame);
    draw_payline();
    draw_result_area();
    draw_bot_bar();
}

/* =====================================================================
 * Per-frame render (incremental: only redraw what changed)
 * ===================================================================== */
static void render(uint32_t frame) {
    if (g_dirty) {
        full_redraw(frame);
        for (uint8_t r = 0; r < N_REELS; r++)
            g_prev_sym[r] = reel_mid_sym(r, frame);
        g_result_dirty = 0;
        g_dirty = 0;
        return;
    }

    /* Only redraw reels whose displayed symbol actually changed */
    uint8_t any_drawn = 0;
    for (uint8_t r = 0; r < N_REELS; r++) {
        uint8_t sym = reel_mid_sym(r, frame);
        if (sym != g_prev_sym[r]) {
            draw_reel(r, frame);
            g_prev_sym[r] = sym;
            any_drawn = 1;
        }
    }
    /* Payline sits on top of reels — only refresh when a reel was redrawn */
    if (any_drawn) draw_payline();

    /* Result area is static between state changes — don't clear+redraw every frame */
    if (g_result_dirty) {
        draw_result_area();
        g_result_dirty = 0;
    }

    /* HUD/bar change rarely — refresh every ~3 seconds */
    if ((frame % 90u) == 0u) { draw_top_bar(); draw_bot_bar(); }
}

/* =====================================================================
 * Public API
 * ===================================================================== */
void slots_init(void) {
    g_state    = ST_IDLE;
    g_timer    = 0;
    g_last_btn = 0;
    g_win_type = 0;
    g_spins    = nv_read(NV_KEY_SPINS, 0);   /* restore from flash */
    g_wins     = nv_read(NV_KEY_WINS,  0);
    g_dirty        = 1;
    g_result_dirty = 0;
    for (uint8_t r = 0; r < N_REELS; r++) {
        g_reels[r].sym     = (uint8_t)(r * 3);  /* staggered start */
        g_reels[r].target  = 0;
        g_reels[r].stopped = 1;
        g_prev_sym[r]      = 0xFF;              /* force initial draw */
    }
}

void slots_update(uint32_t frame, uint8_t btn_pressed) {
    uint8_t btn_edge = (btn_pressed && !g_last_btn) ? 1u : 0u;
    g_last_btn = btn_pressed;

    /* ── State machine ─────────────────────────────────────────────── */
    switch (g_state) {

    case ST_IDLE:
        if (btn_edge) {
            /* Pick targets for all reels */
            for (uint8_t r = 0; r < N_REELS; r++) {
                g_reels[r].target  = rand_sym();
                g_reels[r].stopped = 0;
            }
            g_spins++;
            nv_write(NV_KEY_SPINS, g_spins);
            g_timer  = 0;
            g_state  = ST_SPINNING;
            g_dirty  = 1;
        }
        break;

    case ST_SPINNING:
        if (++g_timer >= 60u) {   /* 60 frames = ~2 s at 30 fps */
            g_timer = 0;
            g_state = ST_STOP_0;
            g_dirty = 1;
        }
        break;

    case ST_STOP_0:
        if (++g_timer >= 20u) {
            g_reels[0].stopped = 1;
            g_timer = 0;
            g_state = ST_STOP_1;
            /* No dirty: reel 0 stopped but no bar change */
        }
        break;

    case ST_STOP_1:
        if (++g_timer >= 20u) {
            g_reels[1].stopped = 1;
            g_timer = 0;
            g_state = ST_STOP_2;
        }
        break;

    case ST_STOP_2:
        if (++g_timer >= 20u) {
            g_reels[2].stopped = 1;
            /* Evaluate result */
            g_win_type = check_win(g_reels[0].target,
                                   g_reels[1].target,
                                   g_reels[2].target);
            if (g_win_type > 0) {
                g_wins++;
                nv_write(NV_KEY_WINS, g_wins);
            }
            g_timer = 0;
            g_state = ST_RESULT;
            g_dirty = 1;
        }
        break;

    case ST_RESULT:
        if (++g_timer >= 90u) {
            g_timer = 0;
            g_state = ST_IDLE;
            g_dirty = 1;
        }
        /* Let player re-spin early during result */
        if (btn_edge && g_timer > 15u) {
            for (uint8_t r = 0; r < N_REELS; r++) {
                g_reels[r].target  = rand_sym();
                g_reels[r].stopped = 0;
            }
            g_spins++;
            nv_write(NV_KEY_SPINS, g_spins);
            g_timer = 0;
            g_state = ST_SPINNING;
            g_dirty = 1;
        }
        break;
    }

    /* ── Render ────────────────────────────────────────────────────── */
    render(frame);
}

void slots_reset_stats(void) {
    g_spins = 0;
    g_wins  = 0;
    g_dirty = 1;   /* redraw top bar with zeroed counters */
}

void slots_wake(void) {
    g_dirty = 1;   /* force full redraw after display sleep */
}
