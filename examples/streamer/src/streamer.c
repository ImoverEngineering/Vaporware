/* streamer.c — SWD frame-streaming display firmware for N32G031 + GC9107
 *
 * Initialization is copied EXACTLY from flappy.c main() — the proven-working
 * baseline.  The only addition is the SWD streaming protocol loop in place of
 * the game loop.
 *
 * Clock: 8 MHz HSI throughout.  SPI1 at 4 MHz (BR_DIV2 of APB2=8MHz).
 *   SPI speed experiments: 24 MHz (PLL/BR_DIV2) and 12 MHz (PLL/BR_DIV4) both
 *   cause flashing white/black display — the PCB trace geometry limits reliable
 *   SPI to 4 MHz regardless of the GC9107 spec (which allows 32 MHz).
 *
 * Half-resolution double-buffer layout — 64×80 logical pixels, 2× scaled:
 *
 *   CTRL       @ 0x20000010  0xDEAD0000 = reset display     (4 bytes)
 *   IDX_A      @ 0x20000100  chunk index for buffer A        (4 bytes)
 *   BUF_A      @ 0x20000104  64×8 logical px = 1024 B        (1024 bytes)
 *   TRIG_A     @ 0x20000504  0xCC = buffer A ready           (4 bytes) <- written LAST
 *   IDX_B      @ 0x20000508  chunk index for buffer B        (4 bytes)
 *   BUF_B      @ 0x2000050C  64×8 logical px = 1024 B        (1024 bytes)
 *   TRIG_B     @ 0x2000090C  0xCC = buffer B ready           (4 bytes) <- written LAST
 *
 * MCU calls display_draw_chunk_2x() — scales 64×8 → 128×16 display pixels.
 * At 12 MHz SPI each 128×16 blit takes ~2.7 ms vs ~11 ms PC write of 1032 B.
 * MCU is always done long before PC revisits the same buffer — no handshake.
 *
 * Stack: 0x20002000 (top) down — ~7408 B available (plenty).
 */

#include "n32g031.h"
#include "display.h"
#include "system.h"
#include "battery.h"

/* Legacy control word */
#define CTRL  ((volatile uint32_t *)0x20000010UL)

/* Buffer A — 64×8 logical pixels = 1024 bytes */
#define IDX_A  ((volatile uint32_t *)0x20000100UL)
#define BUF_A  ((const uint16_t     *)0x20000104UL)
#define TRIG_A ((volatile uint32_t *)0x20000504UL)

/* Buffer B — 64×8 logical pixels = 1024 bytes */
#define IDX_B  ((volatile uint32_t *)0x20000508UL)
#define BUF_B  ((const uint16_t     *)0x2000050CUL)
#define TRIG_B ((volatile uint32_t *)0x2000090CUL)

#define CTRL_IDLE  0x00000000UL
#define CTRL_CHUNK 0x000000CCUL
#define CTRL_RESET 0xDEAD0000UL
#define CTRL_SLEEP 0xDEAD0001UL

#define CHUNK_ROWS  8u    /* logical rows per chunk */
#define CHUNK_W    64u    /* logical pixels per row (display_draw_chunk_2x scales to 128) */
#define NUM_CHUNKS 10u    /* 10 × 8 logical rows = 80 → 160 display rows after 2× scale */

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
    clock_init();              /* 8 MHz HSI, TIM3 PSC=7 → 1 ms ticks         */
    delay_ms(50);
    display_init();            /* 4 MHz SPI — hard limit of this board's traces*/
    display_set_backlight(80);
    tim1_init();
    bat_init();            /* ADC init — included because flappy includes it  */

    /* ── Init protocol SRAM ─────────────────────────────────────────────── */
    *CTRL   = CTRL_IDLE;
    *TRIG_A = CTRL_IDLE;
    *TRIG_B = CTRL_IDLE;

    /* Show startup screen while waiting for the PC to connect */
    draw_waiting();

    /* ── Streaming loop (double-buffer ping-pong) ────────────────────────── */
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
            *CTRL   = CTRL_IDLE;
            *TRIG_A = CTRL_IDLE;
            *TRIG_B = CTRL_IDLE;
            display_init();
            display_set_backlight(80);
            draw_waiting();
            continue;
        }

        /* Buffer A: 64×8 logical chunk, 2× scaled to 128×16 display pixels. */
        if (*TRIG_A == CTRL_CHUNK) {
            uint32_t idx = *IDX_A;
            if (idx < NUM_CHUNKS) {
                uint16_t log_row = (uint16_t)(idx * CHUNK_ROWS);
                display_draw_chunk_2x(BUF_A, log_row, CHUNK_W, CHUNK_ROWS);
            }
            *TRIG_A = CTRL_IDLE;
        }

        /* Buffer B: identical, independent of A. */
        if (*TRIG_B == CTRL_CHUNK) {
            uint32_t idx = *IDX_B;
            if (idx < NUM_CHUNKS) {
                uint16_t log_row = (uint16_t)(idx * CHUNK_ROWS);
                display_draw_chunk_2x(BUF_B, log_row, CHUNK_W, CHUNK_ROWS);
            }
            *TRIG_B = CTRL_IDLE;
        }
    }
}
