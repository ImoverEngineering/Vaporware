/* display_stub.c — SDL2 backend for display.h (slot machine sim) */
#include "display.h"
#include <SDL2/SDL.h>
#include <stdint.h>

#define SCALE 3

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;

/* COL_RGB(r,g,b) packs as BGR565: bits[15:11]=B, bits[10:5]=G, bits[4:0]=R */
static void set_color(uint16_t c) {
    uint8_t r = (uint8_t)((c & 0x1Fu)        << 3);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3Fu) << 2);
    uint8_t b = (uint8_t)((c >> 11)           << 3);
    SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
}

void display_init(void) {
    SDL_Init(SDL_INIT_VIDEO);
    g_win = SDL_CreateWindow(
        "Vaporware Slots  [SPACE=spin]  [Q=quit]",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        128 * SCALE, 160 * SCALE, 0);
    g_ren = SDL_CreateRenderer(g_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
}

void display_fill(uint16_t color) {
    set_color(color);
    SDL_RenderClear(g_ren);
}

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    set_color(color);
    SDL_Rect r = { (int)x * SCALE, (int)y * SCALE,
                   (int)w * SCALE, (int)h * SCALE };
    SDL_RenderFillRect(g_ren, &r);
}

void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    set_color(color);
    SDL_Rect r = { (int)x * SCALE, (int)y * SCALE, SCALE, SCALE };
    SDL_RenderFillRect(g_ren, &r);
}

void display_draw_image(const uint16_t *data, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h) {
    for (uint16_t dy = 0; dy < h; dy++)
        for (uint16_t dx = 0; dx < w; dx++)
            display_draw_pixel((uint16_t)(x+dx), (uint16_t)(y+dy), data[dy*w+dx]);
}

void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
    { (void)x0;(void)y0;(void)x1;(void)y1; }
void display_set_backlight(uint8_t on) { (void)on; }
void display_sleep_in(void)  {}
void display_sleep_out(void) {}

/* Called by sim_main after each frame */
void sim_present(void) { SDL_RenderPresent(g_ren); }
