/* streamer.c — SWD frame-streaming display firmware for N32G031 + GC9107
 *
 * Initialization is copied EXACTLY from flappy.c main() — the proven-working
 * baseline.  The only addition is the SWD streaming protocol loop in place of
 * the game loop.
 *
 * Clock: 8 MHz HSI throughout.  No PLL boost.  SPI runs at 4 MHz.
 *   This matches flappy exactly and guarantees display_init() works.
 *
 * SRAM layout (one write_memory per chunk):
 *
 *   CTRL      @ 0x20000010  0xDEAD0000 = reset display
 *   FAST_IDX  @ 0x20000100  chunk index 0-9         (4 bytes)
 *   FAST_BUF  @ 0x20000104  pixel data 128x16x2 B   (4096 bytes)
 *   FAST_TRIG @ 0x20001104  0xCC = chunk ready       (4 bytes) <- written LAST
 *
 * The PC sends a single write_memory(FAST_IDX_ADDR, 32, {idx <1024 words> 0xCC}).
 * FAST_TRIG is the last word written, so the MCU cannot act before the full
 * chunk is resident in SRAM.
 */

#include "n32g031.h"
#include "display.h"
#include "system.h"
#include "battery.h"

/* Protocol SRAM addresses */
#define CTRL      ((volatile uint32_t *)0x20000010UL)
#define FAST_IDX  ((volatile uint32_t *)0x20000100UL)
#define FAST_BUF  ((const uint16_t     *)0x20000104UL)
#define FAST_TRIG ((volatile uint32_t *)0x20001104UL)

#define CTRL_IDLE  0x00000000UL
#define CTRL_CHUNK 0x000000CCUL
#define CTRL_RESET 0xDEAD0000UL
#define CTRL_SLEEP 0xDEAD0001UL

#define CHUNK_ROWS 16u
#define NUM_CHUNKS 10u

/* draw_waiting — shown at startup while waiting for the PC to start streaming.
 * Fills the entire screen bright magenta so it is unmistakable; overlays three
 * coloured bars in the centre of the screen. */
static void draw_waiting(void)
{
    /* Full-screen bright fill — impossible to confuse with "off" or "black" */
    display_fill(COL_RGB(200, 0, 200));           /* magenta background */

    /* Three coloured bars, centred vertically */
    display_fill_rect(12,  55, 104, 20, COL_RGB(255, 255,   0)); /* yellow */
    display_fill_rect(12,  80, 104, 20, COL_RGB(  0, 255, 255)); /* cyan   */
    display_fill_rect(12, 105, 104, 20, COL_RGB(255, 255, 255)); /* white  */
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

    /* ── Core hardware init (identical to flappy.c) ─────────────────────── */
    clock_init();          /* 8 MHz HSI, TIM3 PSC=7 → 1 ms ticks             */
    delay_ms(50);
    display_init();        /* 4 MHz SPI — same as flappy, guaranteed to work  */
    display_set_backlight(80);
    tim1_init();
    bat_init();            /* ADC init — included because flappy includes it  */

    /* ── Init protocol SRAM ─────────────────────────────────────────────── */
    *CTRL      = CTRL_IDLE;
    *FAST_TRIG = CTRL_IDLE;

    /* Show startup screen while waiting for the PC to connect */
    draw_waiting();

    /* ── Streaming loop ──────────────────────────────────────────────────── */
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

        /* Main path: PC has written IDX + BUF, then set TRIG via mww.
         * Read IDX and BUF immediately — no slow operations before reading them. */
        if (*FAST_TRIG == CTRL_CHUNK) {
            uint32_t idx = *FAST_IDX;
            if (idx < NUM_CHUNKS) {
                uint16_t row_start = (uint16_t)(idx * CHUNK_ROWS);
                display_draw_chunk_cpu(FAST_BUF, row_start, CHUNK_ROWS);
            }
            *FAST_TRIG = CTRL_IDLE;
        }
    }
}
