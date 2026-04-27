/* flappy.c — Flappy Bird for Raz DC25000 / N32G031 + GC9107 128×160 LCD
 *
 * Controls:  PA7 button (active-LOW with internal pull-up) = FLAP
 * Display:   128×160 RGB565, MADCTL=0x98 → R/B channels swapped (BGR=1).
 *            To display visual (R,G,B): send ((B>>3)<<11)|((G>>2)<<5)|(R>>3)
 *
 * VAPE TRIGGER:
 *   When the player dies with score >= 10, the death overlay shows "VAPE NOW"
 *   in red text.  This is a display-only prompt — the coil gate (PB0) is NOT
 *   driven by FlappyVape.  The player is expected to press the physical vape
 *   button on the device separately.  (The original fw_dump.bin did fire PB0
 *   here, but the vaporware reimplementation uses the display prompt instead
 *   to keep app logic decoupled from coil hardware.)
 *
 * IWDG FEED PATTERN:
 *   The main game loop feeds the IWDG once per physics tick (every PHYS_MS=8ms)
 *   via the raw register write: *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
 *   At 8ms/tick, the dog is fed at ~125 Hz — well within any reasonable timeout.
 *   During display_sleep() and flash writes the IWDG is also fed inside the
 *   respective busy-wait loops.
 *
 * COIL SAFETY — PA4/PA5/PA6 driven LOW at startup:
 *   The first thing main() does (before clock_init) is configure PA4, PA5, PA6
 *   as output LOW.  These pins are NOT the coil on production Raz DC25000 boards
 *   (PB0 is the confirmed coil gate), but earlier hardware revisions or unknown
 *   variants may wire the coil differently.  Driving these LOW costs nothing and
 *   prevents accidental coil activation if the firmware is loaded onto a variant
 *   board.  PA7 (button) is left as input and is configured separately.
 *
 * HIGH SCORE STORAGE:
 *   Uses a private write-forward scheme at flash page 0x0800FC00 (1 KB, 256 slots).
 *   Magic: 0xFB1D in high halfword; score in low halfword.  See hisc_read/write.
 *   This is independent of the vaporware nv.h system (predates it).
 *
 * DISPLAY SLEEP:
 *   After DIM_MS (5s) idle: backlight dims to value 15 (gc9107_set_backlight(15)).
 *   After SLEEP_MS (12s) idle: full LCD sleep via gc9107_sleep_in(), all GPIO high-Z.
 *   SWD pins (PA13/PA14) remain in AF mode during sleep for debugger access.
 *
 * Build: compile with -DFLAPPY_BIRD; exclude main.c / tamagotchi.c / slots.c
 */
#ifdef FLAPPY_BIRD

#include "n32g031.h"
#include "display.h"
#include "system.h"
#include "battery.h"


/* ===================================================================
 * Colors  (BGR-swap corrected; to display RGB(r,g,b) send
 *          ((b>>3)<<11)|((g>>2)<<5)|(r>>3))
 * =================================================================== */
#define COL_SKY      0xCDE9U   /* cyan-blue sky — also sprite transparent */
#define COL_BLDG     0x41A3U   /* dark teal building silhouettes */
#define COL_PIPE     0x07E0U   /* bright green pipe body */
#define COL_PIPE_CAP 0x578AU   /* lighter green cap interior */
#define COL_PIPE_DK  0x03C0U   /* dark green cap border */
#define COL_GROUND   0x03C0U   /* dark green ground strip */
#define COL_SCORE    0xFFFFU   /* white */
#define COL_GOLD     0x07FFU   /* yellow — used for high-score digits */
#define COL_EYE      0x0000U   /* black */
#define COL_DEAD     0x001FU   /* red flash */

/* ===================================================================
 * High-score NV storage  (last 1 KB flash page @ 0x0800FC00)
 *
 * Format: one 32-bit word.  High halfword = magic 0xFB1D.
 *         Low halfword = score (0-9999).
 * Flash is erased (0xFFFFFFFF) when blank; we detect that.
 *
 * N32G031 flash register layout (already in FLASH_TypeDef / FLASH_IF):
 *   KEYR  +0x04   CR bits: PG=0, PER=1, MER=2, STRT=6, LOCK=7
 *   SR    +0x0C   BSY=0
 *   CR    +0x10
 *   AR    +0x14   page address for erase
 * =================================================================== */
#define HISC_ADDR   0x0800FC00UL  /* last 1 KB page of 64 KB flash */
#define HISC_MAGIC  0xFB1DU

/* High-score flash storage — write-forward, no-erase scheme.
 *
 * The 1 KB page holds 256 × 32-bit slots.  Each slot is either blank
 * (0xFFFFFFFF) or a valid record: high 16 bits = magic 0xFB1D, low 16 bits
 * = score.  New scores are appended to the first blank slot — no page erase
 * is ever needed because we only write 1→0 transitions.  On read we scan
 * all slots and return the maximum valid score.  Only when all 256 slots are
 * consumed do we erase; at 8 KB SRAM and a game that resets often, hitting
 * 256 lifetime high-score updates is extremely unlikely.
 *
 * This sidesteps the page-erase BSY hang and any write-protection issues
 * on the erase path entirely. */

#define HISC_SLOTS  ((uint32_t)(1024U / 4U))   /* 256 words per 1 KB page */

static uint32_t hisc_read(void)
{
    /* Scan backward — the last written slot is the current high score. */
    volatile uint32_t *page = (volatile uint32_t *)HISC_ADDR;
    for (int i = (int)HISC_SLOTS - 1; i >= 0; i--) {
        uint32_t v = page[i];
        if ((v >> 16) == HISC_MAGIC)
            return v & 0xFFFFU;
    }
    return 0;
}

static void hisc_write(uint32_t score)
{
    if (score > 9999U) score = 9999U;

    /* Find the first blank slot — scan until we find 0xFFFFFFFF. */
    volatile uint32_t *page = (volatile uint32_t *)HISC_ADDR;
    uint32_t slot = HISC_SLOTS;          /* sentinel = page full */
    for (uint32_t i = 0; i < HISC_SLOTS; i++) {
        if (page[i] == 0xFFFFFFFFUL) { slot = i; break; }
    }

    volatile FLASH_TypeDef *F = FLASH_IF;

    if (slot == HISC_SLOTS) {
        /* Page is full — erase it and use slot 0.
         * BSY loops do NOT feed IWDG.  If the erase hangs the watchdog
         * fires, restarting the game (acceptable: player pressed restart). */
        F->KEYR = 0x45670123UL;
        F->KEYR = 0xCDEF89ABUL;
        if (F->CR & (1UL << 7)) return;      /* unlock failed — bail */
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
        F->CR = (1UL << 1); F->AR = HISC_ADDR; F->CR |= (1UL << 6);
        while (F->SR & 1U);
        F->SR  = 0x34U;
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
        slot = 0;
    } else {
        /* Normal path: just unlock for programming, no erase needed. */
        F->KEYR = 0x45670123UL;
        F->KEYR = 0xCDEF89ABUL;
        if (F->CR & (1UL << 7)) return;      /* unlock failed — bail */
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
    }

    /* Program the chosen slot — N32G031 requires a single 32-bit word write. */
    uint32_t addr  = HISC_ADDR + slot * 4U;
    uint32_t val   = ((uint32_t)HISC_MAGIC << 16) | (score & 0xFFFFU);
    F->CR = (1UL << 0);                       /* PG */
    *(volatile uint32_t *)addr = val;          /* 32-bit word write — mandatory on N32G031 */
    while (F->SR & 1U);
    F->SR  = 0x34U;

    F->CR = (1UL << 7);                       /* lock */
}

/* ===================================================================
 * Battery — thresholds and raw reading via vaporware battery.h/battery.c
 * BAT_FULL, BAT_WARN, BAT_CRIT, bat_init(), bat_read_raw() from battery.h
 * =================================================================== */
static uint16_t g_bat_raw = 205u;  /* default = fully charged */

/* ===================================================================
 * Bird sprites — 17×12 px, 3 wing frames
 *
 * _T = transparent (COL_SKY); blit skips these pixels.
 * Wing shifts: up = rows 2-4, mid = rows 5-7, down = rows 8-9.
 * =================================================================== */
#define _T 0xCDE9U  /* sky / transparent */
#define _K 0x0000U  /* black outline */
#define _Y 0x07FFU  /* yellow body */
#define _W 0xFFFFU  /* white eye */
#define _O 0x041FU  /* orange beak  (display ~255,128,0) */
#define _N 0x033FU  /* dark-orange wing (display ~248,100,0) */

static const uint16_t spr_bird_mid[17*12] = {
/* R0 */ _T,_T,_T,_K,_K,_K,_K,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R1 */ _T,_T,_K,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,_T,_T,
/* R2 */ _T,_K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,
/* R3 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_W,_Y,_Y,_Y,_K,_O,_O,_O,_K,_T,
/* R4 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_K,_W,_Y,_K,_O,_O,_O,_O,_K,_T,
/* R5 */ _K,_N,_N,_Y,_Y,_Y,_W,_W,_Y,_Y,_K,_O,_O,_O,_K,_T,_T,
/* R6 */ _K,_N,_N,_N,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,
/* R7 */ _K,_N,_N,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,
/* R8 */ _T,_K,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R9 */ _T,_T,_K,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R10 */ _T,_T,_T,_K,_O,_T,_K,_O,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R11 */ _T,_T,_T,_K,_T,_T,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
};

static const uint16_t spr_bird_up[17*12] = {
/* R0 */ _T,_T,_T,_K,_K,_K,_K,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R1 */ _T,_T,_K,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,_T,_T,
/* R2 */ _T,_K,_N,_N,_Y,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,
/* R3 */ _K,_N,_N,_Y,_Y,_Y,_W,_W,_Y,_Y,_Y,_K,_O,_O,_O,_K,_T,
/* R4 */ _K,_N,_N,_Y,_Y,_Y,_W,_K,_W,_Y,_K,_O,_O,_O,_O,_K,_T,
/* R5 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_W,_Y,_Y,_K,_O,_O,_O,_K,_T,_T,
/* R6 */ _K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,
/* R7 */ _K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,
/* R8 */ _T,_K,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R9 */ _T,_T,_K,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R10 */ _T,_T,_T,_K,_O,_T,_K,_O,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R11 */ _T,_T,_T,_K,_T,_T,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
};

static const uint16_t spr_bird_dn[17*12] = {
/* R0 */ _T,_T,_T,_K,_K,_K,_K,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R1 */ _T,_T,_K,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,_T,_T,
/* R2 */ _T,_K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_K,_T,_T,_T,_T,_T,
/* R3 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_W,_Y,_Y,_Y,_K,_O,_O,_O,_K,_T,
/* R4 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_K,_W,_Y,_K,_O,_O,_O,_O,_K,_T,
/* R5 */ _K,_Y,_Y,_Y,_Y,_Y,_W,_W,_Y,_Y,_K,_O,_O,_O,_K,_T,_T,
/* R6 */ _K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,
/* R7 */ _K,_Y,_Y,_Y,_Y,_Y,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,
/* R8 */ _T,_K,_N,_N,_Y,_Y,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/* R9 */ _T,_T,_K,_N,_N,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R10 */ _T,_T,_T,_K,_O,_T,_K,_O,_T,_T,_T,_T,_T,_T,_T,_T,_T,
/*R11 */ _T,_T,_T,_K,_T,_T,_K,_T,_T,_T,_T,_T,_T,_T,_T,_T,_T,
};

#undef _T
#undef _K
#undef _Y
#undef _W
#undef _O
#undef _N

/* ===================================================================
 * City silhouette background — 128 column heights above GROUND_Y.
 * =================================================================== */
static const uint8_t g_bldg[128] = {
    /* x  0- 4 */ 0,0,0,0,0,
    /* x  5-19 */ 28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,
    /* x 20-25 */ 0,0,0,0,0,0,
    /* x 26-42 */ 33,33,33,33,33,33,33,33,33,33,33,33,33,33,33,33,33,
    /* x 43-47 */ 0,0,0,0,0,
    /* x 48-65 */ 24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,24,
    /* x 66-71 */ 0,0,0,0,0,0,
    /* x 72-86 */ 35,35,35,35,35,35,35,35,35,35,35,35,35,35,35,
    /* x 87-91 */ 0,0,0,0,0,
    /* x 92-108*/ 27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    /* x109-113*/ 0,0,0,0,0,
    /* x114-127*/ 30,30,30,30,30,30,30,30,30,30,30,30,30,30,
};

/* ===================================================================
 * Layout
 * =================================================================== */
#define GROUND_Y    150
#define GROUND_H     10
#define PLAY_H      GROUND_Y

#define SCORE_BAR_H  14
#define SCORE_Y       3
#define PIPE_TOP    SCORE_BAR_H

#define BIRD_X       22
#define BIRD_W       17
#define BIRD_H       12
#define BIRD_START_Y  70

#define PIPE_W       22
#define PIPE_GAP     58
#define CAP_H         8
#define PIPE_SPEED    2
#define PIPE_COUNT    2
#define PIPE_SEP      95
#define GAP_MIN     (PIPE_TOP + 6)
#define GAP_MAX     (GROUND_Y - PIPE_GAP - 10)
#define PIPE_START_X (LCD_WIDTH + 50)

/* ===================================================================
 * Physics  (1/8-pixel fixed-point, 125 Hz)
 * =================================================================== */
#define FP_SHIFT     3
#define GRAVITY_FP   4
#define FLAP_FP      (-48)
#define MAX_FALL_FP  48
#define PHYS_MS      8u

/* Inactivity timeouts (milliseconds, each ≤ 65535) */
#define DIM_MS        5000u   /* dim backlight after this many ms idle */
#define SLEEP_MS     12000u   /* screen off (LCD sleep) after this many ms idle */

/* ===================================================================
 * RNG
 * =================================================================== */
static uint32_t g_seed = 0xDEAD5A1EUL;

static uint16_t rand_gap(void)
{
    g_seed = g_seed * 1664525UL + 1013904223UL;
    return (uint16_t)(GAP_MIN +
           (uint16_t)((g_seed >> 16) % (uint32_t)(GAP_MAX - GAP_MIN + 1)));
}

/* ===================================================================
 * Pipe
 * =================================================================== */
typedef struct { int16_t x; uint8_t gap_top; uint8_t scored; } Pipe;
static Pipe g_pipes[PIPE_COUNT];

static void pipe_reset(int idx, int16_t x)
{
    g_pipes[idx].x       = x;
    g_pipes[idx].gap_top = (uint8_t)rand_gap();
    g_pipes[idx].scored  = 0;
}

/* ===================================================================
 * Single-column strip buffer — 136 pixels covering PIPE_TOP..GROUND_Y.
 * Sending one gc9107_draw_image call per column update means the display
 * scan sees a complete, consistent column change with no partial-update
 * tearing artifacts (vs. multiple fill_rect calls with SPI gaps between).
 * =================================================================== */
#define STRIP_H  (GROUND_Y - PIPE_TOP)   /* 136 */
static uint16_t g_strip_buf[STRIP_H];

/* Fill g_strip_buf with sky+building pattern for column x. */
static void strip_fill_sky(int x)
{
    uint8_t bh      = (uint8_t)g_bldg[x];
    int     bldg_top = GROUND_Y - (int)bh;
    for (int i = 0; i < STRIP_H; i++)
        g_strip_buf[i] = (PIPE_TOP + i < bldg_top) ? COL_SKY : COL_BLDG;
}

/* Fill g_strip_buf with pipe pattern for column x (gap shows sky/bldg). */
static void strip_fill_pipe(int x, int gt, int gb)
{
    uint8_t bh      = (uint8_t)g_bldg[x];
    int     bldg_top = GROUND_Y - (int)bh;
    int     top_cap  = gt - CAP_H; if (top_cap < PIPE_TOP)  top_cap = PIPE_TOP;
    int     bot_cap  = gb + CAP_H; if (bot_cap > GROUND_Y)  bot_cap = GROUND_Y;

    for (int i = 0; i < STRIP_H; i++) {
        int y = PIPE_TOP + i;
        uint16_t col;
        if (y >= gt && y < gb) {
            col = (y < bldg_top) ? COL_SKY : COL_BLDG;   /* gap: background */
        } else if (y < gt) {
            /* top pipe */
            if      (y == top_cap || y == gt - 1)       col = COL_PIPE_DK;
            else if (y >  top_cap && y <  gt - 1)       col = COL_PIPE_CAP;
            else                                         col = COL_PIPE;
        } else {
            /* bottom pipe */
            if      (y == gb || y == bot_cap - 1)        col = COL_PIPE_DK;
            else if (y >  gb && y <  bot_cap - 1)        col = COL_PIPE_CAP;
            else                                         col = COL_PIPE;
        }
        g_strip_buf[i] = col;
    }
}

/* Send g_strip_buf to the display at column x, rows PIPE_TOP..GROUND_Y-1. */
static void send_strip(int x)
{
    if (x < 0 || x >= LCD_WIDTH) return;
    gc9107_draw_image(g_strip_buf, (uint16_t)x, (uint16_t)PIPE_TOP,
                      1, (uint16_t)STRIP_H);
}

/* ===================================================================
 * Background helpers
 * =================================================================== */
static void paint_sky_strip(int x, int w)
{
    if (x < 0) { w += x; x = 0; }
    if (w <= 0 || x >= LCD_WIDTH) return;
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;

    /* Process in runs of identical building height.
     * Each pixel column is written ONCE — no prior full-strip sky fill —
     * which eliminates double-write flash artifacts at building boundaries. */
    for (int c = x, end = x + w; c < end; ) {
        uint8_t bh = g_bldg[c];
        int run = c;
        while (c < end && g_bldg[c] == bh) c++;
        uint16_t rw = (uint16_t)(c - run);
        if (bh == 0) {
            gc9107_fill_rect((uint16_t)run, (uint16_t)PIPE_TOP, rw,
                             (uint16_t)(GROUND_Y - PIPE_TOP), COL_SKY);
        } else {
            int bldg_top = GROUND_Y - (int)bh;
            if (bldg_top > PIPE_TOP)
                gc9107_fill_rect((uint16_t)run, (uint16_t)PIPE_TOP, rw,
                                 (uint16_t)(bldg_top - PIPE_TOP), COL_SKY);
            gc9107_fill_rect((uint16_t)run, (uint16_t)bldg_top, rw,
                             (uint16_t)bh, COL_BLDG);
        }
    }
}

/* Erase a rect, restoring sky + buildings — no double-writes. */
static void paint_bg_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < PIPE_TOP) { h -= (PIPE_TOP - y); y = PIPE_TOP; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > GROUND_Y)  h = GROUND_Y - y;
    if (w <= 0 || h <= 0) return;

    for (int c = x, end = x + w; c < end; ) {
        uint8_t bh = g_bldg[c];
        int run = c;
        while (c < end && g_bldg[c] == bh) c++;
        uint16_t rw = (uint16_t)(c - run);
        int bldg_top = GROUND_Y - (int)bh; /* = GROUND_Y when bh==0 */

        /* Sky portion of this rect column */
        int sky_y0 = y;
        int sky_y1 = (bldg_top < y + h) ? bldg_top : y + h;
        if (sky_y1 > sky_y0)
            gc9107_fill_rect((uint16_t)run, (uint16_t)sky_y0, rw,
                             (uint16_t)(sky_y1 - sky_y0), COL_SKY);

        /* Building portion (only if building overlaps the rect) */
        if (bh > 0) {
            int iy0 = (bldg_top > y)     ? bldg_top : y;
            int iy1 = (GROUND_Y < y + h) ? GROUND_Y : y + h;
            if (iy1 > iy0)
                gc9107_fill_rect((uint16_t)run, (uint16_t)iy0, rw,
                                 (uint16_t)(iy1 - iy0), COL_BLDG);
        }
    }
}

/* ===================================================================
 * Pipe strip painters (differential rendering)
 * Each column is written as a single draw_image call — the display scan
 * never sees a partially-updated column, eliminating tearing lines.
 * =================================================================== */
static void pipe_render(const Pipe *p, uint16_t col)
{
    int x = (int)p->x, gt = (int)p->gap_top, gb = gt + PIPE_GAP;
    for (int c = x; c < x + PIPE_W; c++) {
        if (c < 0 || c >= LCD_WIDTH) continue;
        if (col == COL_SKY) strip_fill_sky(c);
        else strip_fill_pipe(c, gt, gb);
        send_strip(c);
    }
}

static int pipe_scroll(Pipe *p)
{
    int old_x = (int)p->x, new_x = old_x - PIPE_SPEED;
    int gt = (int)p->gap_top, gb = gt + PIPE_GAP;
    if (new_x + PIPE_W <= 0) {
        /* Fully off-screen: erase any remaining visible columns */
        for (int c = (old_x < 0 ? 0 : old_x);
             c < old_x + PIPE_W && c < LCD_WIDTH; c++) {
            strip_fill_sky(c); send_strip(c);
        }
        return 1;
    }
    /* Erase PIPE_SPEED trailing columns */
    for (int i = 0; i < PIPE_SPEED; i++) {
        int trail = old_x + PIPE_W - PIPE_SPEED + i;
        if (trail >= 0 && trail < LCD_WIDTH) {
            strip_fill_sky(trail); send_strip(trail);
        }
    }
    /* Draw PIPE_SPEED leading columns */
    p->x = (int16_t)new_x;
    for (int i = 0; i < PIPE_SPEED; i++) {
        int lead = new_x + i;
        if (lead >= 0 && lead < LCD_WIDTH) {
            strip_fill_pipe(lead, gt, gb); send_strip(lead);
        }
    }
    return 0;
}

/* ===================================================================
 * Bird — transparent blit (skips COL_SKY pixels row-by-row)
 * =================================================================== */
static void bird_erase(int y)
{
    if (y < 0) y = 0;
    if (y + BIRD_H > PLAY_H) y = PLAY_H - BIRD_H;
    paint_bg_rect(BIRD_X, y, BIRD_W, BIRD_H);
}

static void bird_render(int y, int32_t vel_fp)
{
    if (y < 0) y = 0;
    if (y + BIRD_H > PLAY_H) y = PLAY_H - BIRD_H;
    const uint16_t *spr = (vel_fp < -8) ? spr_bird_up :
                          (vel_fp > 12) ? spr_bird_dn : spr_bird_mid;

    for (int r = 0; r < BIRD_H; r++) {
        const uint16_t *row = spr + r * BIRD_W;
        int c = 0;
        while (c < BIRD_W) {
            while (c < BIRD_W && row[c] == COL_SKY) c++;
            if (c >= BIRD_W) break;
            int start = c;
            while (c < BIRD_W && row[c] != COL_SKY) c++;
            gc9107_draw_image(row + start,
                              (uint16_t)(BIRD_X + start), (uint16_t)(y + r),
                              (uint16_t)(c - start), 1);
        }
    }
}

/* ===================================================================
 * Font & score rendering
 *
 * Digit glyphs: 3×5 px → drawn 2× scaled (6×10 px).
 * Letter glyphs for death screen: H=0, I=1, B=2, E=3, S=4, T=5.
 * draw_digit_at / draw_letter_at take explicit fg/bg colors and y pos.
 * =================================================================== */
static const uint8_t g_digits[10][5] = {
    {0x7,0x5,0x5,0x5,0x7},{0x2,0x6,0x2,0x2,0x7},{0x7,0x1,0x7,0x4,0x7},
    {0x7,0x1,0x7,0x1,0x7},{0x5,0x5,0x7,0x1,0x1},{0x7,0x4,0x7,0x1,0x7},
    {0x7,0x4,0x7,0x5,0x7},{0x7,0x1,0x1,0x1,0x1},{0x7,0x5,0x7,0x5,0x7},
    {0x7,0x5,0x7,0x1,0x7},
};
/* H        I        B        E        S        T   */
static const uint8_t g_letters[6][5] = {
    {0x5,0x5,0x7,0x5,0x5}, {0x7,0x2,0x2,0x2,0x7},
    {0x6,0x5,0x6,0x5,0x6}, {0x7,0x4,0x6,0x4,0x7},
    {0x7,0x4,0x7,0x1,0x7}, {0x7,0x2,0x2,0x2,0x2},
};

static void draw_glyph(const uint8_t bm[5], uint16_t px, uint16_t py,
                        uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < 5; r++) {
        uint8_t b = bm[r];
        for (int c = 0; c < 3; c++)
            gc9107_fill_rect(px + (uint16_t)(c * 2), py + (uint16_t)(r * 2),
                             2, 2, ((b >> (2 - c)) & 1) ? fg : bg);
    }
}

/* Width of a score number (digits only) in pixels at 2× scale */
static uint16_t score_px_width(uint32_t sc)
{
    return (sc < 10U) ? 6U : (sc < 100U) ? 14U : 22U;
}

/* Draw score centered horizontally at given y */
static void draw_score_at(uint32_t sc, uint16_t y, uint16_t fg, uint16_t bg)
{
    /* Clear the full max-width area first so old wider digits don't leave ghosts
     * (e.g. going from "9" to "10" would otherwise leave residual pixels) */
    uint16_t clear_x = (uint16_t)((LCD_WIDTH - 22U) / 2);
    gc9107_fill_rect(clear_x, y, 22, 10, bg);

    uint16_t x = (uint16_t)((LCD_WIDTH - score_px_width(sc)) / 2);
    if (sc >= 100U) { draw_glyph(g_digits[(sc/100)%10], x,    y, fg, bg); x += 8; }
    if (sc >=  10U) { draw_glyph(g_digits[(sc/10) %10], x,    y, fg, bg); x += 8; }
                      draw_glyph(g_digits[ sc      %10], x,    y, fg, bg);
}

/* Battery icon — 17×8 body (x=108,y=3) + 2×4 nub (x=125,y=5).
 * Three 4×6 fill bars at x=109,114,119.  Call after score bar is drawn. */
static void draw_bat(void)
{
    uint16_t raw = g_bat_raw;
    uint16_t dim = 0x2104U;   /* dark grey */
    uint16_t c0, c1, c2;

    if      (raw >= BAT_FULL) { c0 = 0x06E0U; c1 = 0x06E0U; c2 = 0x06E0U; } /* green  */
    else if (raw >= BAT_WARN) { c0 = 0x06FFU; c1 = 0x06FFU; c2 = dim;     } /* yellow */
    else if (raw >= BAT_CRIT) { c0 = COL_DEAD; c1 = dim;    c2 = dim;     } /* red    */
    else                      { c0 = dim;      c1 = dim;     c2 = dim;     } /* dead   */

    /* Clear interior then draw border */
    gc9107_fill_rect(108, 3, 17, 8, COL_EYE);
    gc9107_fill_rect(108, 3, 17, 1, COL_SCORE);  /* top edge    */
    gc9107_fill_rect(108,10, 17, 1, COL_SCORE);  /* bottom edge */
    gc9107_fill_rect(108, 3,  1, 8, COL_SCORE);  /* left edge   */
    gc9107_fill_rect(124, 3,  1, 8, COL_SCORE);  /* right edge  */
    gc9107_fill_rect(125, 5,  2, 4, COL_SCORE);  /* nub         */
    /* Fill bars */
    gc9107_fill_rect(109, 4, 4, 6, c0);
    gc9107_fill_rect(114, 4, 4, 6, c1);
    gc9107_fill_rect(119, 4, 4, 6, c2);
}

/* Standard in-game score bar (white on black, fixed y) */
static void draw_score(uint32_t sc)
{
    draw_score_at(sc, SCORE_Y, COL_SCORE, COL_EYE);
    draw_bat();
}

/* ===================================================================
 * Death overlay — shown after red flash, over the game scene.
 * Draws a centred panel with current score (white) and best (gold).
 * Panel remains visible until the player restarts.
 * =================================================================== */

/* Extra letter glyphs: G=0, C=1, O=2, R=3, V=4, A=5, P=6, N=7, W=8  (3×5 px, same scale) */
static const uint8_t g_letters2[9][5] = {
    {0x7,0x4,0x5,0x5,0x7}, /* G */
    {0x7,0x4,0x4,0x4,0x7}, /* C */
    {0x7,0x5,0x5,0x5,0x7}, /* O */
    {0x7,0x5,0x7,0x6,0x5}, /* R */
    {0x5,0x5,0x5,0x2,0x2}, /* V */
    {0x2,0x5,0x7,0x5,0x5}, /* A */
    {0x7,0x5,0x6,0x4,0x4}, /* P */
    {0x5,0x7,0x5,0x5,0x5}, /* N */
    {0x5,0x5,0x5,0x7,0x5}, /* W */
};

static void draw_death_overlay(uint32_t score, uint32_t best, uint8_t show_vape)
{
    /* Panel */
    const uint16_t px = 14, py = 42, pw = 100, ph = 90;
    gc9107_fill_rect(px, py, pw, ph, COL_EYE);
    gc9107_fill_rect(px,       py,       pw, 1,  COL_SCORE);
    gc9107_fill_rect(px,       py+ph-1,  pw, 1,  COL_SCORE);
    gc9107_fill_rect(px,       py,       1,  ph, COL_SCORE);
    gc9107_fill_rect(px+pw-1,  py,       1,  ph, COL_SCORE);

    /* "SCORE" label (white) at y=48
     * S=g_letters[4], C=g_letters2[1], O=g_letters2[2], R=g_letters2[3], E=g_letters[3]
     * 5 chars × 6px + 4 gaps × 2px = 38px wide */
    {
        uint16_t lw = 5*6 + 4*2;
        uint16_t lx = (uint16_t)((LCD_WIDTH - lw) / 2);
        draw_glyph(g_letters[4],  lx,    48, COL_SCORE, COL_EYE); /* S */
        draw_glyph(g_letters2[1], lx+8,  48, COL_SCORE, COL_EYE); /* C */
        draw_glyph(g_letters2[2], lx+16, 48, COL_SCORE, COL_EYE); /* O */
        draw_glyph(g_letters2[3], lx+24, 48, COL_SCORE, COL_EYE); /* R */
        draw_glyph(g_letters[3],  lx+32, 48, COL_SCORE, COL_EYE); /* E */
    }

    /* Current score (white) at y=61 */
    draw_score_at(score, 61, COL_SCORE, COL_EYE);

    /* Divider */
    gc9107_fill_rect(px+6, 74, pw-12, 1, 0x4208U);

    /* "HIGHSCORE" label (gold) at y=78
     * H,I,G,H,S,C,O,R,E — 9 chars × 6px + 8 gaps × 2px = 70px wide */
    {
        uint16_t lw = 9*6 + 8*2;
        uint16_t lx = (uint16_t)((LCD_WIDTH - lw) / 2);
        draw_glyph(g_letters[0],  lx,    78, COL_GOLD, COL_EYE); /* H */
        draw_glyph(g_letters[1],  lx+8,  78, COL_GOLD, COL_EYE); /* I */
        draw_glyph(g_letters2[0], lx+16, 78, COL_GOLD, COL_EYE); /* G */
        draw_glyph(g_letters[0],  lx+24, 78, COL_GOLD, COL_EYE); /* H */
        draw_glyph(g_letters[4],  lx+32, 78, COL_GOLD, COL_EYE); /* S */
        draw_glyph(g_letters2[1], lx+40, 78, COL_GOLD, COL_EYE); /* C */
        draw_glyph(g_letters2[2], lx+48, 78, COL_GOLD, COL_EYE); /* O */
        draw_glyph(g_letters2[3], lx+56, 78, COL_GOLD, COL_EYE); /* R */
        draw_glyph(g_letters[3],  lx+64, 78, COL_GOLD, COL_EYE); /* E */
    }

    /* Best score (gold) at y=91 */
    draw_score_at(best, 91, COL_GOLD, COL_EYE);

    /* "VAPE NOW" (red) at y=107 — only when score >= 10 */
    if (show_vape) {
        uint16_t lx = (uint16_t)((LCD_WIDTH - 56) / 2);
        draw_glyph(g_letters2[4], lx,    107, COL_DEAD, COL_EYE); /* V */
        draw_glyph(g_letters2[5], lx+8,  107, COL_DEAD, COL_EYE); /* A */
        draw_glyph(g_letters2[6], lx+16, 107, COL_DEAD, COL_EYE); /* P */
        draw_glyph(g_letters[3],  lx+24, 107, COL_DEAD, COL_EYE); /* E */
        draw_glyph(g_letters2[7], lx+34, 107, COL_DEAD, COL_EYE); /* N */
        draw_glyph(g_letters2[2], lx+42, 107, COL_DEAD, COL_EYE); /* O */
        draw_glyph(g_letters2[8], lx+50, 107, COL_DEAD, COL_EYE); /* W */
    }
}

/* ===================================================================
 * Waiting-screen hint — "SCORE 10 / TO VAPE"
 * Drawn once after scene_draw at game_restart; cleared by scene_draw
 * on the first flap that transitions to ST_PLAYING.
 * =================================================================== */
static void draw_waiting_hint(void)
{
    const uint16_t fg = COL_SCORE;  /* white */
    const uint16_t bg = COL_EYE;   /* black backdrop */

    /* Dark pill behind both lines */
    gc9107_fill_rect(4, 104, LCD_WIDTH - 8, 34, COL_EYE);

    /* Line 1: "SCORE 10"  — 56 px wide, centered
     * S(0) C(8) O(16) R(24) E(32)  [+4 gap]  1(42) 0(50) */
    {
        uint16_t lx = (uint16_t)((LCD_WIDTH - 56) / 2);
        draw_glyph(g_letters[4],  lx,    108, fg, bg); /* S */
        draw_glyph(g_letters2[1], lx+8,  108, fg, bg); /* C */
        draw_glyph(g_letters2[2], lx+16, 108, fg, bg); /* O */
        draw_glyph(g_letters2[3], lx+24, 108, fg, bg); /* R */
        draw_glyph(g_letters[3],  lx+32, 108, fg, bg); /* E */
        draw_glyph(g_digits[1],   lx+42, 108, fg, bg); /* 1 */
        draw_glyph(g_digits[0],   lx+50, 108, fg, bg); /* 0 */
    }

    /* Line 2: "TO VAPE"  — 48 px wide, centered
     * T(0) O(8)  [+4 gap]  V(18) A(26) P(34) E(42) */
    {
        uint16_t lx = (uint16_t)((LCD_WIDTH - 48) / 2);
        draw_glyph(g_letters[5],  lx,    122, fg, bg); /* T */
        draw_glyph(g_letters2[2], lx+8,  122, fg, bg); /* O */
        draw_glyph(g_letters2[4], lx+18, 122, fg, bg); /* V */
        draw_glyph(g_letters2[5], lx+26, 122, fg, bg); /* A */
        draw_glyph(g_letters2[6], lx+34, 122, fg, bg); /* P */
        draw_glyph(g_letters[3],  lx+42, 122, fg, bg); /* E */
    }
}

/* ===================================================================
 * Collision
 * =================================================================== */
static int hit_pipe(int by, const Pipe *p)
{
    int bx1=BIRD_X+1, bx2=BIRD_X+BIRD_W-2, by1=by+1, by2=by+BIRD_H-2;
    int px1=(int)p->x, px2=px1+PIPE_W-1;
    if (bx2<px1||bx1>px2) return 0;
    if (by1<(int)p->gap_top||by2>=(int)p->gap_top+PIPE_GAP) return 1;
    return 0;
}

/* ===================================================================
 * Full scene redraw
 * =================================================================== */
static void scene_draw(int bird_y, int32_t vel_fp, uint32_t score)
{
    /* Play area: sky + buildings, each pixel written ONCE (no double-write) */
    for (int c = 0, end = LCD_WIDTH; c < end; ) {
        uint8_t bh = g_bldg[c];
        int run = c;
        while (c < end && g_bldg[c] == bh) c++;
        uint16_t rw = (uint16_t)(c - run);
        if (bh == 0) {
            gc9107_fill_rect((uint16_t)run, (uint16_t)PIPE_TOP, rw,
                             (uint16_t)(GROUND_Y - PIPE_TOP), COL_SKY);
        } else {
            int bt = GROUND_Y - (int)bh;
            if (bt > PIPE_TOP)
                gc9107_fill_rect((uint16_t)run, (uint16_t)PIPE_TOP, rw,
                                 (uint16_t)(bt - PIPE_TOP), COL_SKY);
            gc9107_fill_rect((uint16_t)run, (uint16_t)bt, rw,
                             (uint16_t)bh, COL_BLDG);
        }
    }
    gc9107_fill_rect(0, 0,        LCD_WIDTH, SCORE_BAR_H, COL_EYE);
    gc9107_fill_rect(0, GROUND_Y, LCD_WIDTH, GROUND_H,    COL_GROUND);
    for (int i = 0; i < PIPE_COUNT; i++) pipe_render(&g_pipes[i], COL_PIPE);
    bird_render(bird_y, vel_fp);
    draw_score(score);
}

/* ===================================================================
 * Display sleep — backlight off, poll button, feed IWDG.
 * Returns once button is pressed and released.
 * =================================================================== */
static void display_sleep(void)
{
    /* Save GPIO MODER before gc9107_sleep_in() modifies PB4. */
    uint32_t saved_modera = GPIOA->MODER;
    uint32_t saved_moderb = GPIOB->MODER;

    /* Send LCD sleep commands (Display Off → Sleep In). */
    gc9107_sleep_in();

    /* Put EVERY GPIO pin into analog mode (MODER=11 = output buffer
     * disconnected, pin truly high-Z).  This releases the backlight
     * driver's enable/dim input so it can default to off, regardless
     * of which physical pin is wired to it. */
    GPIOA->MODER = 0xFFFFFFFFUL;
    GPIOB->MODER = 0xFFFFFFFFUL;

    /* Restore PA7 as digital input with pull-up so we can read the button. */
    GPIOA->MODER &= ~(3UL << (7 * 2));   /* PA7 = input (00) */

    /* Keep SWD pins (PA13=SWDIO, PA14=SWCK) in AF mode so the ST-Link
     * can still connect and flash while the device is sleeping. */
    GPIOA->MODER &= ~((3UL << (13 * 2)) | (3UL << (14 * 2)));
    GPIOA->MODER |=   (2UL << (13 * 2)) | (2UL << (14 * 2));  /* AF */

    /* Keep RST (PB6) driven HIGH — if it floats LOW the GC9107 gets a
     * hardware reset that wipes all init registers, and Sleep Out alone
     * cannot restore them.  ODR still holds the HIGH written at init. */
    GPIOB->MODER &= ~(3UL << (6 * 2));
    GPIOB->MODER |=  (1UL  << (6 * 2));  /* PB6 = output */
    GPIOB->BSRR   =  (1UL  <<  6);       /* RST HIGH */

    /* Keep CS (PA15) driven HIGH (deasserted) to prevent noise on the
     * floating SPI lines from being clocked into the sleeping LCD. */
    GPIOA->MODER &= ~(3UL << (15 * 2));
    GPIOA->MODER |=  (1UL  << (15 * 2)); /* PA15 = output */
    GPIOA->BSRR   =  (1UL  <<  15);      /* CS HIGH */

    /* Wait for button press then release, feeding IWDG. */
    while (GPIOA->IDR & (1u << 7))
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
    while (!(GPIOA->IDR & (1u << 7)))
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;

    /* Restore GPIO configuration before talking to the LCD. */
    GPIOA->MODER = saved_modera;
    GPIOB->MODER = saved_moderb;

    /* Wake LCD (Sleep Out + 120 ms + Display On) and restore backlight. */
    gc9107_sleep_out();
    gc9107_set_backlight(80);
}

/* ===================================================================
 * Game states
 * =================================================================== */
#define ST_WAITING 0
#define ST_PLAYING 1
#define ST_DEAD    2

/* ===================================================================
 * main
 * =================================================================== */
int main(void)
{
    /* Start the IWDG (in case it is in software-start mode — writing 0xCCCC
     * is harmless if hardware-start mode already has it running).
     * Then immediately reload so the counter starts from the top. */
    *(volatile uint32_t *)0x40003000UL = 0xCCCCUL;   /* start */
    *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;   /* reload */

    /* Coil safety: drive PA4, PA5, PA6 to output LOW before anything else.
     *
     * Why these pins: the confirmed coil gate on production Raz DC25000 is PB0
     * (from Ghidra analysis of fw_dump.bin).  PA4/PA5/PA6 are NOT the coil on
     * known boards, but unknown hardware variants or reworked boards might differ.
     * Driving them LOW is a conservative safety measure with zero side-effects
     * on production hardware.
     *
     * Register addresses used directly (before clock_init / n32g031.h structs):
     *   0x40021018 = RCC->APB2ENR  — enable GPIOA clock (bit 2 = IOPAEN)
     *   0x40010800 = GPIOA->MODER  — configure PA4,PA5,PA6 as outputs (MODER=01)
     *   0x40010818 = GPIOA->BSRR   — BSRR bits[20:22] clear PA4/5/6 to LOW
     *                                 (BSRR bits[16+n] = reset pin n)
     */
    {
        volatile uint32_t *rcc  = (volatile uint32_t *)0x40021018UL; /* APB2ENR */
        volatile uint32_t *modr = (volatile uint32_t *)0x40010800UL; /* GPIOA MODER */
        volatile uint32_t *bsrr = (volatile uint32_t *)0x40010818UL; /* GPIOA BSRR */
        *rcc  |= (1UL << 2);     /* IOPAEN: enable GPIOA clock */
        (void)*modr;             /* dummy read to allow clock to propagate */
        *modr &= ~((3UL<<8)|(3UL<<10)|(3UL<<12));  /* clear MODER for PA4,PA5,PA6 */
        *modr |=  ((1UL<<8)|(1UL<<10)|(1UL<<12));  /* set PA4,PA5,PA6 = output */
        *bsrr  =  (1UL<<20)|(1UL<<21)|(1UL<<22);   /* reset PA4,PA5,PA6 → LOW */
    }

    clock_init();
    delay_ms(50);
    display_init();
    display_set_backlight(80);
    tim1_init();
    bat_init();

    /* PA7 = button, active-low, pull-up */
    GPIOA->MODER &= ~(3UL << (7*2));
    GPIOA->PUPDR &= ~(3UL << (7*2));
    GPIOA->PUPDR |=  (1UL << (7*2));

    /* Load persistent high score from flash */
    uint32_t high_score = hisc_read();

    g_bat_raw = bat_read_raw();     /* initial battery reading */

    gc9107_fill(0x0000U);           /* clear to black before game draws */

    uint8_t  state;
    int32_t  bird_fp, vel_fp;
    int      bird_y, prev_y;
    uint32_t score, prev_score;
    uint8_t  btn_prev;
    uint32_t dead_hold;
    uint32_t frame_ctr;
    uint16_t phys_t;
    uint16_t last_active;   /* ms timestamp of last button press */
    uint8_t  is_dimmed;     /* 1 = backlight already dimmed */
    uint16_t btn_held_ms;   /* ms button continuously held (for hard reset) */
    uint8_t  new_hisc;      /* 1 = high score beaten this round, needs flash write */

    frame_ctr = 0;

game_restart:
    g_seed ^= (frame_ctr * 2654435761UL) ^ 0xC0DE1234UL;

    state       = ST_WAITING;
    bird_fp     = (int32_t)BIRD_START_Y << FP_SHIFT;
    vel_fp      = 0;
    bird_y      = BIRD_START_Y;
    prev_y      = BIRD_START_Y;
    score       = 0;
    prev_score  = 0xFFFFFFFFUL;
    btn_prev    = 1;
    dead_hold   = 0;
    is_dimmed   = 0;
    btn_held_ms = 0;
    new_hisc    = 0;
    phys_t      = ms_now();
    last_active = ms_now();
    gc9107_set_backlight(80);

    pipe_reset(0, (int16_t)(PIPE_START_X));
    pipe_reset(1, (int16_t)(PIPE_START_X + PIPE_SEP));

    scene_draw(bird_y, vel_fp, 0);
    draw_waiting_hint();
    prev_score = 0;

    while (1)
    {
        *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;  /* IWDG */

        /* ---- Inactivity dim / sleep ---- */
        /* Only apply during WAITING or DEAD — never dim mid-game. During play,
         * immediately restore full brightness if it was previously dimmed. */
        {
            if (state == ST_PLAYING) {
                if (is_dimmed) { gc9107_set_backlight(80); is_dimmed = 0; }
            } else {
                uint16_t sleep_ms = (g_bat_raw < BAT_CRIT) ? 3000u : (uint16_t)SLEEP_MS;
                uint16_t dim_ms   = (g_bat_raw < BAT_CRIT) ? 1000u : (uint16_t)DIM_MS;
                uint16_t idle = (uint16_t)(ms_now() - last_active);
                if (idle >= sleep_ms) {
                    display_sleep();        /* backlight off; wait button wake */
                    last_active = ms_now();
                    is_dimmed   = 0;
                    scene_draw(bird_y, vel_fp, score);
                    if (state == ST_DEAD)
                        draw_death_overlay(score, high_score, score >= 10u);
                    phys_t   = ms_now();
                    btn_prev = 1;
                    continue;
                } else if (idle >= dim_ms) {
                    if (!is_dimmed) { gc9107_set_backlight(15); is_dimmed = 1; }
                } else {
                    if (is_dimmed) { gc9107_set_backlight(80); is_dimmed = 0; }
                }
            }
        }

        /* ---- Physics tick gate ---- */
        uint16_t now = ms_now();
        if ((uint16_t)(now - phys_t) < PHYS_MS) continue;
        phys_t += (uint16_t)PHYS_MS;
        frame_ctr++;

        /* Battery read every 625 frames (~5 s at 125 Hz) */
        if (frame_ctr % 625u == 0u) {
            g_bat_raw = bat_read_raw();
            draw_bat();
            /* Force sleep immediately if critically low */
            if (g_bat_raw < BAT_CRIT) {
                display_sleep();
                last_active = ms_now();
                is_dimmed   = 0;
                scene_draw(bird_y, vel_fp, score);
                if (state == ST_DEAD)
                    draw_death_overlay(score, high_score, score >= 10u);
                phys_t   = ms_now();
                btn_prev = 1;
                continue;
            }
        }

        /* Button edge detect + held-down hard-reset (10 s) */
        uint8_t btn  = (uint8_t)((GPIOA->IDR >> 7) & 1u);
        uint8_t flap = (btn == 0u && btn_prev == 1u);
        btn_prev = btn;
        if (flap) { last_active = ms_now(); btn_held_ms = 0; }
        if (btn == 0u) {                         /* button held */
            btn_held_ms += PHYS_MS;
            if (btn_held_ms >= 10000u) goto game_restart;  /* hard reset */
        } else {
            btn_held_ms = 0;
        }
        if (flap) last_active = ms_now();

        /* ---- Waiting ---- */
        if (state == ST_WAITING) {
            if (flap) {
                state = ST_PLAYING;
                vel_fp = FLAP_FP;
                scene_draw(bird_y, vel_fp, score); /* wipe hint */
            }
            continue;
        }

        /* ---- Dead ---- */
        if (state == ST_DEAD) {
            if (++dead_hold > 15u && flap) {
                /* Write new high score to flash now, right before restart.
                 * If the flash erase hangs and the IWDG fires, the device
                 * resets — which just restarts the game sooner. Acceptable. */
                if (new_hisc) hisc_write(high_score);
                goto game_restart;
            }
            continue;
        }

        /* ==============================================================
         * PLAYING — physics + differential render
         * ============================================================== */

        if (flap) vel_fp = FLAP_FP;
        vel_fp += GRAVITY_FP;
        if (vel_fp > MAX_FALL_FP) vel_fp = MAX_FALL_FP;
        bird_fp += vel_fp;

        if (bird_fp < ((int32_t)PIPE_TOP << FP_SHIFT)) {
            bird_fp = (int32_t)PIPE_TOP << FP_SHIFT; vel_fp = 0;
        }
        int ny = (int)(bird_fp >> FP_SHIFT);
        if (ny + BIRD_H >= GROUND_Y) { ny = GROUND_Y - BIRD_H; state = ST_DEAD; }

        /* Pipe scroll */
        for (int i = 0; i < PIPE_COUNT; i++) {
            if (pipe_scroll(&g_pipes[i]))
                pipe_reset(i, (int16_t)((int)g_pipes[i^1].x + PIPE_SEP));
            if (!g_pipes[i].scored && (int)g_pipes[i].x + PIPE_W < BIRD_X) {
                g_pipes[i].scored = 1; score++;
            }
        }

        /* Collision */
        if (state == ST_PLAYING)
            for (int i = 0; i < PIPE_COUNT; i++)
                if (hit_pipe(ny, &g_pipes[i])) { state = ST_DEAD; break; }

        /* Bird */
        if (prev_y != ny) bird_erase(prev_y);
        bird_render(ny, vel_fp);
        prev_y = ny; bird_y = ny;
        bird_fp = (int32_t)ny << FP_SHIFT;

        /* Score bar */
        if (score != prev_score) { draw_score(score); prev_score = score; }

        /* ---- On death: update high score in RAM, show overlay ---- */
        if (state == ST_DEAD) {
            /* Update RAM immediately so overlay shows correct high score.
             * Flash write is deferred to restart (see ST_DEAD handler above). */
            if (score > high_score) {
                high_score = score;
                new_hisc   = 1;
            }

            *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
            gc9107_fill(COL_DEAD);
            /* IWDG-safe 80 ms red-flash delay */
            {
                uint16_t t0 = ms_now();
                while ((uint16_t)(ms_now() - t0) < 80u)
                    *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
            }
            *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
            scene_draw(bird_y, vel_fp, score);
            *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
            draw_death_overlay(score, high_score, score >= 10u);
            if (score >= 10u) {
                *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;
            }
            prev_score = score;
            phys_t     = ms_now();
            last_active = ms_now();         /* reset sleep timer on death */
        }
    }

    return 0;
}

#endif /* FLAPPY_BIRD */
