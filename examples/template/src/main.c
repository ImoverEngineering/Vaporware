/* template/src/main.c — Vaporware SDK application template
 *
 * Copy the entire template/ directory and rename it for your new app.
 * This file is the ONLY file you need to write.  The framework handles:
 *   - Hardware init (clocks, display, SPI, button, battery, IWDG)
 *   - ~30 fps frame loop with IWDG feeding
 *   - Auto-sleep after configurable idle timeout
 *   - Hold-button-to-reset with configurable duration and callback
 *   - Persistent NV storage (survives power cycles)
 *   - Safe deep sleep (all GPIO high-Z; SWD stays live for reflashing)
 *
 * You implement three functions:
 *   app_init()       — called once after all hardware is up
 *   app_update()     — called every frame (~33 ms)
 *   app_wake()       — called after wake-from-sleep (optional: redraw screen)
 *
 * Available APIs:
 *   display_*()      — fill, fill_rect, draw_pixel, draw_image (display.h)
 *   button_*()       — pressed, just_pressed, held_ms            (button.h)
 *   nv_read/write()  — persistent flash storage, key/value u32   (nv.h)
 *   bat_read_raw()   — 12-bit ADC battery reading                 (battery.h)
 *   bat_level(raw)   — 0-3 charge bars                           (battery.h)
 *   delay_ms()       — blocking delay, feeds IWDG                 (system.h)
 *   ms_now()         — 16-bit millisecond counter, wraps ~65 s    (system.h)
 */
#include "app.h"
#include "display.h"
#include "button.h"
#include "nv.h"
#include "battery.h"
#include "system.h"

/* ── NV keys for this app ─────────────────────────────────────────────
 * Use the pre-allocated app keys (NV_KEY_APP_0..2) for your own data.
 * See nv.h for all available keys.                                    */
#define MY_KEY  NV_KEY_APP_0

/* ── App state ──────────────────────────────────────────────────────── */
static uint32_t g_count = 0;

/* ── Hold-to-reset callback ─────────────────────────────────────────── */
static void on_reset(void) {
    g_count = 0;
    nv_write(MY_KEY, 0);
}

/* ── Framework callbacks ────────────────────────────────────────────── */

void app_init(void) {
    /* Configure framework features */
    app_set_sleep_timeout(20000);           /* sleep after 20 s idle    */
    app_set_hold_reset(10000, on_reset);    /* hold 10 s to reset       */

    /* Restore persistent state */
    g_count = nv_read(MY_KEY, 0);

    /* Draw initial screen */
    display_fill(COL_RGB(0, 0, 0));        /* black background          */
    /* TODO: draw your UI here */
}

void app_update(uint32_t frame) {
    (void)frame;    /* remove cast if you use frame for animation timing */

    if (button_just_pressed()) {
        g_count++;
        nv_write(MY_KEY, g_count);

        /* TODO: update your UI here */
        display_fill(COL_RGB(0, 0, 32));   /* dark blue background      */
    }
}

void app_wake(void) {
    /* Called after the device wakes from sleep.
     * Redraw the full screen — display RAM is intact but SPI lines were
     * floating, so pixels may be corrupted.                            */
    display_fill(COL_RGB(0, 0, 0));
    /* TODO: redraw your UI here */
}
