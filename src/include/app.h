/* vaporware/include/app.h — Application framework
 *
 * Arduino-style app model: the framework provides main() and handles all
 * hardware init, IWDG, frame timing, sleep, and button management.
 * Apps implement two functions and optionally register callbacks.
 *
 * Minimal app skeleton:
 * ─────────────────────────────────────────────────────────────────────
 *   #include "app.h"
 *   #include "display.h"
 *
 *   void app_init(void) {
 *       app_set_sleep_timeout(20000);            // sleep after 20 s idle
 *       app_set_hold_reset(10000, my_reset_fn);  // 10 s hold → callback
 *       display_fill(COL_BLACK);                 // initial draw
 *   }
 *
 *   void app_update(uint32_t frame) {
 *       if (button_just_pressed()) { ... }
 *   }
 *
 *   // Optional — called after waking from sleep:
 *   void app_wake(void) { display_fill(COL_BLACK); }
 * ─────────────────────────────────────────────────────────────────────
 *
 * Startup sequence performed by the framework (in order):
 *   1. IWDG_START / IWDG_FEED
 *   2. clock_init()
 *   3. delay_ms(50)
 *   4. display_init() + display_set_backlight(1)
 *   5. tim1_init()
 *   6. button_init()
 *   7. app_init()           ← your code starts here
 *   8. frame loop           ← app_update() called every ~33 ms
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "button.h"   /* button_* functions available to all apps */
#include "nv.h"       /* nv_read / nv_write available to all apps */

/* ── Implement these two in your application ─────────────────────── */

/* Called once after all hardware is initialised.
 * Contract:
 *   - May call any display_*, button_*, nv_*, bat_* functions.
 *   - May call app_set_sleep_timeout() and app_set_hold_reset().
 *   - Should draw the initial UI (display_fill + whatever the first frame shows).
 *   - Must return; do NOT spin forever in app_init().
 * Called in hardware-init order: clock, display, TIM1, button, battery all done. */
void app_init(void);

/* Called every frame at ~30 fps (every APP_FRAME_MS = 33 ms by default).
 * Contract:
 *   - button_update() has already been called — query button_pressed() etc. freely.
 *   - frame is a monotonically increasing counter (starts at 0, wraps at UINT32_MAX).
 *   - Must return promptly; overruns reduce frame rate but do not cause errors.
 *   - IWDG is fed by the framework once per frame loop iteration (before this call).
 *     If app_update() itself blocks for > watchdog timeout, the device will reset.
 *     For long blocking operations (flash write, ADC read), call IWDG_FEED() or
 *     use delay_ms() (which feeds IWDG internally). */
void app_update(uint32_t frame);

/* ── Optional weak callback ──────────────────────────────────────── */

/* Called after the device wakes from display sleep (before the next frame).
 * The default implementation is a no-op (weak symbol).
 * Override to trigger a full UI redraw — the display was blanked before sleep,
 * so anything drawn before sleep_in() is no longer visible.
 * Called from within the framework's device_sleep() function in app.c. */
void app_wake(void);

/* ── Framework configuration — call from app_init() ─────────────── */

/* Enable automatic display sleep after idle_ms of no button activity.
 * "Activity" is defined as button_pressed() returning non-zero.
 * On sleep, all GPIO pins go high-Z, display_sleep_in() is called,
 * and the loop blocks until the button is pressed and released.
 * Pass idle_ms=0 to disable auto-sleep entirely. */
void app_set_sleep_timeout(uint16_t idle_ms);

/* Register a hold-to-reset callback.
 * cb() is called exactly once when the button has been held continuously
 * for hold_ms milliseconds.  It will not fire again until the button is
 * fully released and held again from zero.
 * Typical use: pass a function that calls nv_reset() and redraws the UI.
 * Pass hold_ms=0 or cb=NULL to disable. */
void app_set_hold_reset(uint16_t hold_ms, void (*cb)(void));

#endif /* APP_H */
