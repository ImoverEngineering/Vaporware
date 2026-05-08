/* streamer.c — SWD frame-streaming display firmware for N32G031 + GC9107
 *
 * Initialization is copied EXACTLY from flappy.c main() — the proven-working
 * baseline.  The only addition is the SWD streaming protocol loop in place of
 * the game loop.
 *
 * Clock: 8 MHz HSI throughout.  No PLL boost.  SPI runs at 4 MHz.
 *   This matches flappy exactly and guarantees display_init() works.
 *
 * Half-resolution 2× mode (64×80 logical → 128×160 physical):
 *   Each logical pixel is displayed as a 2×2 block by display_draw_chunk_2x().
 *   SWD payload per chunk is 4× smaller (1024 B vs 4096 B), pushing ~6fps
 *   vs ~3fps in full-resolution mode and significantly reducing display tearing.
 *   SPI output per chunk is the same (4096 B) because each logical pixel is
 *   sent four times.
 *
 * SRAM layout (one write_memory per chunk):
 *
 *   CTRL      @ 0x20000010  0xDEAD0000 = reset display
 *   FAST_IDX  @ 0x20000100  chunk index 0-4              (4 bytes)
 *   FAST_BUF  @ 0x20000104  logical pixel data 64×40×2 B (5120 bytes)
 *   FAST_TRIG @ 0x20001504  0xCC = chunk ready            (4 bytes) <- written LAST
 *
 * The PC sends a single write_memory of 5128 bytes to 0x20000100.
 * FAST_TRIG is the last word written, so the MCU cannot act before the full
 * chunk is resident in SRAM.
 *
 * 2-chunk mode cuts USB round-trips to 2 per frame vs the old 5-chunk mode,
 * targeting ~30fps and dramatically reducing the rolling-shutter artifact.
 */

#include "n32g031.h"
#include "display.h"
#include "system.h"
#include "battery.h"

/* Protocol SRAM addresses — 2× pixel mode (64×80 logical → 128×160 physical) */
#define CTRL      ((volatile uint32_t *)0x20000010UL)
#define FAST_IDX  ((volatile uint32_t *)0x20000100UL)
#define FAST_BUF  ((const uint16_t    *)0x20000104UL)  /* 64×40×2 = 5120 bytes */
#define FAST_TRIG ((volatile uint32_t *)0x20001504UL)  /* 0x20000104 + 5120   */

#define CTRL_IDLE  0x00000000UL
#define CTRL_CHUNK 0x000000CCUL
#define CTRL_RESET 0xDEAD0000UL
#define CTRL_SLEEP 0xDEAD0001UL

#define CHUNK_ROWS 40u  /* logical rows per chunk (80 physical rows after 2× scale) */
#define CHUNK_W    64u  /* logical width (128 physical pixels after 2× scale) */
#define NUM_CHUNKS 2u

/* Crash-location sentinels — written before each operation.
 * Read via: openocd -f n32g031.openocd.cfg -c 'init;halt;mdw 0x20000060 4;resume;exit'
 *   0x20000060 = 0x11111111 → entered draw_waiting
 *   0x20000064 = 0x22222222 → magenta fill done
 *   0x20000068 = 0x33333333 → all rects done, draw_waiting returned
 *   0x2000006C = 0x44444444 → reached while(1) */
#define MARK(addr, val) (*(volatile uint32_t *)(addr##UL) = (val##UL))

/* draw_waiting — shown at startup while waiting for the PC to start streaming.
 * Fills the entire screen bright magenta so it is unmistakable; overlays three
 * coloured bars in the centre of the screen. */
static void draw_waiting(void)
{
    MARK(0x20000060, 0xCC110000);          /* entered draw_waiting            */
    display_fill(COL_RGB(200,   0, 200));  /* magenta                         */
    MARK(0x20000064, 0xCC220000);          /* magenta done                    */
    display_fill(COL_RGB(255, 255,   0));  /* yellow                          */
    MARK(0x20000068, 0xCC330000);          /* yellow done                     */
    display_fill(COL_RGB(  0, 255, 255));  /* cyan                            */
    MARK(0x20000074, 0xCC440000);          /* all fills done, draw_waiting ok */
}

int main(void)
{
    /* ── IWDG start (identical to flappy.c) ─────────────────────────────── */
    *(volatile uint32_t *)0x40003000UL = 0xCCCCUL;   /* start  */
    *(volatile uint32_t *)0x40003000UL = 0xAAAAUL;   /* reload */

    /* ── Coil safety: drive PA4, PA5, PA6 LOW (identical to flappy.c) ───── */
    {
        volatile uint32_t *rcc  = (volatile uint32_t *)0x40021018UL; /* APB2ENR */
        volatile uint32_t *modr = (volatile uint32_t *)0x40010800UL; /* GPIOA MODER */
        volatile uint32_t *bsrr = (volatile uint32_t *)0x40010818UL; /* GPIOA BSRR  */
        *rcc  |= (1UL << 2);                                   /* IOPAEN */
        (void)*modr;                                            /* dummy read */
        *modr &= ~((3UL << 8) | (3UL << 10) | (3UL << 12));   /* clear PA4/5/6 */
        *modr |=  ((1UL << 8) | (1UL << 10) | (1UL << 12));   /* PA4/5/6 output */
        *bsrr  =  (1UL << 20) | (1UL << 21) | (1UL << 22);    /* PA4/5/6 LOW    */
    }

    /* ── Core hardware init ─────────────────────────────────────────────── */
    clock_init();          /* 8 MHz HSI, TIM3 PSC=7 → 1 ms ticks             */
    delay_ms(50);
    display_init();        /* 4 MHz SPI — same as flappy, guaranteed to work  */
    display_set_backlight(80);

    /* ── PLL diagnostic: full switch with 2WS flash ─────────────────────
     * GREEN = ran at 48 MHz, show magenta; RED = PLL timed out (8 MHz). */
    clock_boost_48mhz();
    if (RCC->CFGR & RCC_CFGR_SWS_PLL) {
        /* 12 MHz SPI for streaming: 48 MHz PLL / 4 = 12 MHz (GC9107 max ≈16 MHz).
         * Blit time: 4096 bytes × 8 / 12 MHz ≈ 2.7ms — well under the ~10ms
         * PC round-trip, so FAST_BUF is never overwritten while MCU reads it. */
        SPI1->CR1 = (SPI1->CR1 & ~(7UL << 3)) | SPI_CR1_BR_DIV4; /* 12 MHz */
        display_fill(COL_RGB(0, 200, 0));   /* GREEN: 48 MHz active */
    } else {
        display_fill(COL_RGB(200, 0, 0));   /* RED: stayed on 8 MHz */
    }
    delay_ms(2000);

    tim1_init();
    /* bat_init() omitted — streamer never calls bat_read_raw(), and bat_init()
     * writes RCC_CFGR2 which can disturb the PLL at 48 MHz. */

    /* ── Init protocol SRAM ─────────────────────────────────────────────── */
    *CTRL      = CTRL_IDLE;
    *FAST_TRIG = CTRL_IDLE;

    /* Show startup screen while waiting for the PC to connect */
    draw_waiting();

    /* ── Streaming loop ──────────────────────────────────────────────────── */
    MARK(0x20000078, 0x44444444);
    while (1) {
        IWDG_FEED();

        /* PC writes CTRL_SLEEP → turn off LCD, idle so debugger can halt MCU */
        if (*CTRL == CTRL_SLEEP) {
            *CTRL = CTRL_IDLE;
            display_sleep_in();
            while (1) { IWDG_FEED(); }
        }

        /* PC writes CTRL_RESET → re-init display (e.g. after power glitch) */
        if (*CTRL == CTRL_RESET) {
            *CTRL      = CTRL_IDLE;
            *FAST_TRIG = CTRL_IDLE;
            display_init();
            display_set_backlight(80);
            draw_waiting();
            continue;
        }

        /* Main path: PC has written IDX + BUF, then set TRIG via load_image.
         * Read IDX and BUF immediately — no slow operations before reading them.
         * display_draw_chunk_2x scales each logical pixel 2× horizontally and
         * vertically, mapping 64×8 logical pixels to 128×16 physical pixels. */
        if (*FAST_TRIG == CTRL_CHUNK) {
            uint32_t idx = *FAST_IDX;
            if (idx < NUM_CHUNKS) {
                uint16_t log_row = (uint16_t)(idx * CHUNK_ROWS);
                display_draw_chunk_2x(FAST_BUF, log_row, CHUNK_W, CHUNK_ROWS);
            }
            *FAST_TRIG = CTRL_IDLE;
        }
    }
}
