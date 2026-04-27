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
 * Auto-sleep GPIO state:
 *   On sleep, device_sleep() sets EVERY GPIO pin to MODER=11 (analog/high-Z).
 *   Analog mode disconnects the output driver and pull-up/down — the pin truly
 *   floats — regardless of what OTYPER, OSPEEDR, or PUPDR contain.
 *   This ensures no current flows through the backlight LED driver or SPI lines.
 *   Four pins are then restored before the sleep wait loop:
 *     BTN_PORT/BTN_PIN — input so the button can wake the device
 *     PA13/PA14 (SWDIO/SWCLK) — AF mode so debugger stays reachable
 *     LCD_RST_PORT/LCD_RST_PIN — output HIGH (prevents GC9107 hardware reset)
 *     LCD_CS_PORT/LCD_CS_PIN  — output HIGH (blocks SPI noise into sleeping LCD)
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

/* ── Device sleep (hardware-agnostic) ────────────────────────────── */
static void device_sleep(void) {
    /* Fill panel black before killing backlight — prevents white-streak
     * artefacts from floating SPI lines or display RAM noise.          */
    display_fill(0x0000);

    /* Save full GPIO config then put EVERY pin into analog/high-Z.
     * This floats the backlight driver enable input off regardless of
     * which physical pin is wired to it (hardware-agnostic).          */
    uint32_t saved_a = GPIOA->MODER;
    uint32_t saved_b = GPIOB->MODER;
    GPIOA->MODER = 0xFFFFFFFFUL;
    GPIOB->MODER = 0xFFFFFFFFUL;

    /* Restore only the pins needed during sleep: */

    /* Button — input with pull-up still active in PUPDR */
    BTN_PORT->MODER &= ~(3UL << (BTN_PIN * 2));

    /* SWD (PA13=SWDIO, PA14=SWCLK) — AF mode; Cortex-M0 fixed pins.
     * Keep live so a debugger can reconnect without a full power cycle. */
    GPIOA->MODER &= ~((3UL << (SWD_SWDIO_PIN * 2)) | (3UL << (SWD_SWCLK_PIN * 2)));
    GPIOA->MODER |=   (2UL << (SWD_SWDIO_PIN * 2)) | (2UL << (SWD_SWCLK_PIN * 2));

    /* Display RST — output HIGH (holds GC9107 out of reset so its GRAM
     * registers survive sleep without a full re-init on wake).         */
    LCD_RST_PORT->MODER &= ~(3UL << (LCD_RST_PIN * 2));
    LCD_RST_PORT->MODER |=  (1UL  << (LCD_RST_PIN * 2));
    LCD_RST_PORT->BSRR   =  (1UL  << LCD_RST_PIN);

    /* Display CS — output HIGH (deasserted; blocks SPI bus noise).    */
    LCD_CS_PORT->MODER &= ~(3UL << (LCD_CS_PIN * 2));
    LCD_CS_PORT->MODER |=  (1UL  << (LCD_CS_PIN * 2));
    LCD_CS_PORT->BSRR   =  (1UL  << LCD_CS_PIN);

    /* Wait for button press then release (active-low).
     * IWDG is fed continuously — device never bricks in sleep.        */
    while ( (BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); } /* await LOW  */
    while (!(BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); } /* await HIGH */

    /* Restore full GPIO config */
    GPIOA->MODER = saved_a;
    GPIOB->MODER = saved_b;

    /* Notify app to redraw */
    app_wake();
}

/* ── Framework main ───────────────────────────────────────────────── */
int main(void) {
    IWDG_START();
    IWDG_FEED();

    vape_safety_init();

    /* Core hardware init */
    clock_init();
    delay_ms(50);
    display_init();
    display_set_backlight(1);
    tim1_init();
    vape_init();
    button_init();
    bat_init();     /* ADC init; does not alter PB0 GPIO mode */

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
