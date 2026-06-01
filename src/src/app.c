/* vaporware/src/app.c — Application framework
 *
 * Provides main(). Apps implement app_init() and app_update().
 * See app.h for the full design, startup sequence, and contract.
 *
 * Frame timing:
 *   APP_FRAME_MS (config.h, default 33 ms ≈ 30 fps) is the target frame period.
 *   The framework waits until (uint16_t)(ms_now() - last_frame) >= APP_FRAME_MS
 *   before calling app_update().  If app_update() overruns the frame budget, the
 *   next frame fires immediately with no catch-up logic — overruns simply reduce
 *   the effective frame rate.  There is no frame drop or skip mechanism.
 *
 * Hold-to-reset:
 *   app_set_hold_reset(hold_ms, cb) registers a callback fired once when the
 *   button is held for hold_ms consecutive milliseconds.  g_hold_fired prevents
 *   the callback from repeating on subsequent frames while the button stays down.
 *   It resets when the button is released.
 *
 * Sleep protocol (implemented in device_sleep()):
 *   Display VCC is switched by a P-channel MOSFET whose gate is PA4 and/or PA6.
 *   Gate HIGH → FET off → VCC cut → display + backlight lose power.
 *   PA5 is the coil gate on this hardware and must NEVER be driven HIGH.
 *   1. SPI disable + PA4/PA6 HIGH → VCC off → backlight off, screen dark.
 *   2. Poll button until press then release (IWDG fed throughout).
 *   3. display_init(): drives PA4/5/6 LOW (VCC on), re-configures SPI and
 *      display GPIOs, runs RST sequence, sends the full GC9107 init table.
 *   4. display_set_backlight(1): backlight on.
 *   5. app_wake(): app-specific redraw (default no-op).
 */
#include "app.h"
#include "config.h"
#include "system.h"
#include "display.h"
#include "vape.h"
#include "button.h"
#include "battery.h"

/* ── Framework state ──────────────────────────────────────────────── */
static uint16_t g_sleep_ms    = 0;
static uint16_t g_last_active = 0;
static uint16_t g_hold_ms     = 0;
static void   (*g_hold_cb)(void) = (void *)0;
static uint8_t  g_hold_fired  = 0;

/* ── Config API ───────────────────────────────────────────────────── */

void app_set_sleep_timeout(uint16_t idle_ms) {
    g_sleep_ms = idle_ms;
}

void app_set_hold_reset(uint16_t hold_ms, void (*cb)(void)) {
    g_hold_ms = hold_ms;
    g_hold_cb = cb;
}

/* ── Weak default for app_wake ────────────────────────────────────── */
__attribute__((weak)) void app_wake(void) {}

/* ── Device sleep ─────────────────────────────────────────────────── */
static void device_sleep(void) {
    /* ── Step 1: Cut display VCC and disable SPI.
     *   PA4 and PA6 are the gates of the display VCC P-FET (P-channel MOSFET:
     *   gate HIGH → FET off → VCC rail cut → display + backlight lose power).
     *   PA5 is deliberately excluded — it is the coil gate on this hardware
     *   and driving it HIGH fires the heating element.
     *   SPI is disabled first to tristate the SPI pads before VCC is removed,
     *   preventing back-feed through the GC9107's I/O protection diodes.     */
    SPI1->CR1 &= ~SPI_CR1_SPE;
    GPIOA->BSRR = (1UL << 4) | (1UL << 6);   /* PA4, PA6 HIGH = VCC OFF; PA5 untouched */

    /* ── Step 2: Wait for button press then release ───────────────────── */
    while ( (BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); } /* await LOW  */
    while (!(BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); } /* await HIGH */

    /* ── Step 3: Restore VCC and reinitialize display.
     *   display_init() calls display_gpio_init() which drives PA4/5/6 LOW
     *   (VCC on), reconfigures SPI and display GPIOs, runs the RST sequence,
     *   and sends the full GC9107 init table.  GRAM is undefined after a VCC
     *   cut; app_wake() redraws before the next frame to avoid garbage flash. */
    IWDG_FEED();
    display_init();
    display_set_backlight(1);
    app_wake();
}

/* ── Framework main ───────────────────────────────────────────────── */
int main(void) {
    IWDG_START();
    IWDG_FEED();

    vape_safety_init();

    /* Core hardware init */
    clock_init();
    /* OG factory firmware waits ~1-2 s of peripheral init before touching the
     * display.  200 ms here lets the LDO and GC9107 power rails fully stabilise
     * after a fresh MCU reset before we start the RST sequence. */
    IWDG_FEED(); delay_ms(200); IWDG_FEED();

    /* Use RST-only init (display_init) — no VCC power cycle.
     * display_recover() (VCC cut via PA4/5/6) leaves this panel white on some
     * board variants; display_init() with just the RST sequence works correctly. */
    bat_init();
    display_init();
    display_set_backlight(1);
    tim1_init();
    vape_init();
    button_init();


    /* App-specific setup */
    app_init();

    uint32_t frame      = 0;
    uint16_t last_frame = ms_now();
    g_last_active       = ms_now();

    while (1) {
        IWDG_FEED();
        uint16_t now = ms_now();

        /* ── Auto-sleep ───────────────────────────────────────────── */
        if (g_sleep_ms &&
            (uint16_t)(now - g_last_active) >= g_sleep_ms) {
            device_sleep();
            g_last_active = ms_now();
            last_frame    = g_last_active;
        }

        /* ── Frame tick (APP_FRAME_MS ≈ 30 fps) ──────────────────── */
        if ((uint16_t)(now - last_frame) >= APP_FRAME_MS) {
            last_frame = now;

            button_update();

            /* Track activity */
            if (button_pressed()) g_last_active = now;

            /* Hold-to-reset */
            if (g_hold_ms && g_hold_cb && button_held_ms() >= g_hold_ms) {
                if (!g_hold_fired) {
                    g_hold_cb();
                    g_hold_fired = 1;
                }
            } else if (!button_pressed()) {
                g_hold_fired = 0;
            }

            app_update(frame);
            frame++;
        }
    }
}
