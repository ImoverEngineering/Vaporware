/* vaporware/include/display.h — GC9107 128x160 LCD display API
 *
 * Hardware: GC9107 panel, 128x160 RGB565, connected via SPI1 on N32G031
 *   PB3 = SPI1_SCK  PB5 = SPI1_MOSI  PA15 = CS (active-low, software)
 *   PB7 = D/C        PB6 = RST (active-low pulse)
 *   PB4 = backlight  (active-LOW: LOW=on, HIGH=off)
 *
 * Color encoding: MADCTL=0x98 sets BGR=1 (R and B channels are swapped).
 * To display visual color (R,G,B), send: ((B>>3)<<11)|((G>>2)<<5)|(R>>3)
 *
 * Usage:
 *   1. Call clock_init() first (needed for delay_ms used in display_init)
 *   2. Call display_init() once at startup
 *   3. Call display_set_backlight(1) to turn on the backlight
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* Panel dimensions */
#define LCD_WIDTH   128
#define LCD_HEIGHT  160

/* ---- Color helpers ----
 * COL_RGB(r8, g8, b8) — converts 8-bit RGB to BGR-swapped RGB565 for this display
 * RAW_RGB565(r5,g6,b5) — sends raw bit fields directly (no swap)
 */
#define COL_RGB(r,g,b)    ((((b)>>3)<<11)|(((g)>>2)<<5)|((r)>>3))
#define RAW_RGB565(r,g,b) ((((r)&0x1F)<<11)|(((g)&0x3F)<<5)|((b)&0x1F))

/* Common colors (visually correct on this display) */
#define COL_BLACK    0x0000U
#define COL_WHITE    0xFFFFU
#define COL_RED      COL_RGB(255,   0,   0)
#define COL_GREEN    COL_RGB(  0, 255,   0)
#define COL_BLUE     COL_RGB(  0,   0, 255)
#define COL_YELLOW   COL_RGB(255, 255,   0)
#define COL_CYAN     COL_RGB(  0, 255, 255)
#define COL_MAGENTA  COL_RGB(255,   0, 255)
#define COL_ORANGE   COL_RGB(255, 165,   0)

/* ---- API ---- */

/* Initialise GPIO, SPI1, and the GC9107 panel.
 * Requires clock_init() to have been called first (uses delay_ms internally).
 * Side effects:
 *   - Configures PB3/PB5 as SPI1 AF0, PA15/PB6/PB7 as GPIO outputs.
 *   - Drives PB4 (backlight) LOW (on) at the end of init.
 *   - Calls display_fill(0x0000) to flush GRAM — takes ~50ms on SPI.
 * Call display_set_backlight(1) after this if backlight was disabled by the
 * framework (app.c enables it explicitly after display_init returns). */
void display_init(void);

/* Control the PB4 backlight.
 * on=0: HIGH = backlight off (active-LOW circuit: HIGH cuts LED driver)
 * on!=0: LOW = backlight on
 * Plain GPIO — no PWM, no dimming.  Instantaneous, no delay. */
void display_set_backlight(uint8_t on);

/* Fill entire 128×160 screen with color (RGB565, BGR-swapped per MADCTL=0x98).
 * Asserts CS LOW, writes 20480 pixels, deasserts CS HIGH.
 * Blocks for ~50 ms at 4 MHz SPI.  Feeds no IWDG — do not call in watchdog-
 * sensitive contexts longer than the watchdog timeout. */
void display_fill(uint16_t color);

/* Fill a rectangle with color.
 * x, y  — top-left corner in pixels (0-origin, (0,0) = top-left of display)
 * w, h  — width and height in pixels
 * color — RGB565, BGR-swapped (use COL_RGB macro for visual colors)
 * Asserts CS for the duration; deasserts CS on return.
 * Clips are NOT performed — caller must ensure x+w<=128, y+h<=160. */
void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* Set the GRAM write window (column/row address set) and issue RAMWR (0x2C).
 * After this call, CS remains LOW and DC is HIGH (data mode).
 * Caller must write pixel data bytes directly to SPI1->DR, wait for BSY,
 * then deassert CS.  Used internally by fill/image functions.
 *
 * Side effect: CS stays LOW after return — caller MUST manage CS.
 * GC9107 requirement: CS must stay LOW for the entire window+data sequence. */
void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Blit a pre-rendered w×h image to position (x,y).
 * img   — pointer to w*h uint16_t pixels in row-major order (top-left first)
 * x, y  — top-left corner on the display
 * w, h  — image dimensions in pixels
 * color format: RGB565 with BGR swap (same as display_fill_rect).
 * Asserts CS for the duration; deasserts CS on return. */
void display_draw_image(const uint16_t *img, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h);

/* Blit a 128×nrows chunk to (0, row_start) using CPU-polled SPI.
 * Simpler than DMA — no channel-mapping dependency.  At 24 MHz SPI each
 * 128×16 chunk takes ~1.4 ms; during this time SWD writes incur wait-states
 * but still succeed (OpenOCD retries automatically).
 * Asserts and deasserts CS. */
void display_draw_chunk_cpu(const uint16_t *buf, uint16_t row_start, uint16_t nrows);

/* Blit a 128×nrows chunk to (0, row_start) using DMA-driven SPI.
 * buf   — pointer to 128*nrows uint16_t pixels in row-major order (BGR565)
 * row_start — first display row to write (0-origin)
 * nrows     — number of rows in this chunk (typically 16)
 *
 * The CPU idles during the DMA transfer so the AHB bus is free for SWD
 * memory writes.  This eliminates the bus-contention problem that made PLL
 * reduce SWD throughput with the old CPU-polling SPI path.
 *
 * Requires DMA1 clock enabled before first call (RCC->AHBENR |= RCC_AHBENR_DMA1EN).
 * Asserts and deasserts CS. */
void display_draw_chunk_dma(const uint16_t *buf, uint16_t row_start, uint16_t nrows);

/* Blit a log_w × log_h logical chunk, scaling 2× in both axes.
 * Physical output: (0, log_row*2) .. (127, log_row*2 + log_h*2 - 1).
 * Used by the half-resolution SWD streamer (64×80 → 128×160 display).
 * Asserts and deasserts CS. */
void display_draw_chunk_2x(const uint16_t *src, uint16_t log_row,
                            uint16_t log_w, uint16_t log_h);

/* Draw a single pixel at (x,y).
 * Out-of-bounds: silently ignored (x>=128 or y>=160).
 * Asserts and deasserts CS per pixel — use display_fill_rect for areas. */
void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/* Sleep In sequence: backlight off → Display Off (0x28) → Sleep In (0x10).
 * After this call, SPI activity to the panel should stop.
 * Allow ≥5 ms before any further SPI commands. */
void display_sleep_in(void);

/* Sleep Out sequence: Sleep Out (0x11) → 120 ms delay → Display On (0x29) → backlight on.
 * The 120 ms delay is mandated by the GC9107 datasheet after SLPOUT.
 * IWDG is fed during the delay.  Blocks for ~130 ms total. */
void display_sleep_out(void);

/* ---- Legacy aliases — keeps old gc9107_* call sites building ---- */
#define gc9107_init()                    display_init()
#define gc9107_set_backlight(p)          display_set_backlight(p)
#define gc9107_fill(c)                   display_fill(c)
#define gc9107_fill_rect(x,y,w,h,c)     display_fill_rect(x,y,w,h,c)
#define gc9107_set_window(x0,y0,x1,y1)  display_set_window(x0,y0,x1,y1)
#define gc9107_draw_image(i,x,y,w,h)    display_draw_image(i,x,y,w,h)
#define gc9107_draw_pixel(x,y,c)        display_draw_pixel(x,y,c)
#define gc9107_sleep_in()               display_sleep_in()
#define gc9107_sleep_out()              display_sleep_out()

#endif /* DISPLAY_H */
