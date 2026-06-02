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
 *   2. vape_safety_init()   — coil gate forced LOW (safety-critical)
 *   3. clock_init()         — HSI 8 MHz + TIM3 timebase
 *   4. delay_ms(200)        — LDO + GC9107 power-rail settle
 *   5. bat_init()           — ADC for battery reads
 *   6. display_init()       — GPIO, SPI1, GC9107 init table, GRAM clear
 *   7. display_set_backlight(1)
 *   8. tim1_init()          — free-running 1 kHz wall clock (ms_now())
 *   9. vape_init()          — puff sensor / coil hardware
 *  10. button_init()        — PA7 input with pull-up
 *  11. app_init()           ← your code starts here
 *  12. frame loop           ← app_update() called every ~33 ms
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

/* Called after the device wakes from sleep (before the next frame).
 * The default implementation is a no-op (weak symbol).
 * Override to redraw the full screen — display GRAM is cleared to black by
 * display_init() during the wake sequence, so the previous frame is gone.
 * Called from within the framework's device_sleep() function in app.c. */
void app_wake(void);

/* ── Framework configuration — call from app_init() ─────────────── */

/* Enable automatic sleep after idle_ms of no button activity.
 * "Activity" is defined as button_pressed() returning non-zero.
 *
 * Sleep protocol (app.c device_sleep()):
 *   1. SPI disabled + PA4/PA6 HIGH → display VCC cut → screen dark.
 *      (PA5 is never touched — it is the coil gate on some board variants.)
 *   2. MCU enters Stop mode via system_enter_stop() (~10-20 µA on N32G031).
 *      EXTI7 falling edge (PA7 button) is the wake source.
 *      IWDG continues on LSI; MCU resets if button not pressed in ~26 s.
 *   3. Wake: 48 MHz PLL restored, button release waited.
 *   4. display_init(): VCC back on, SPI re-init, full GC9107 init + black fill.
 *   5. display_set_backlight(1): backlight on.
 *   6. app_wake(): your redraw hook fires.
 *
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
