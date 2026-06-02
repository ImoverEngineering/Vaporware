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
 *   2. system_enter_stop(): MCU enters Stop mode (~10-20 µA), wakes on
 *      EXTI7 falling edge (PA7 button press).  Restores 48 MHz PLL on wake.
 *   3. Wait for button release (so wake press is not a game input).
 *   4. display_init(): drives PA4/5/6 LOW (VCC on), re-configures SPI and
 *      display GPIOs, runs RST sequence, sends the full GC9107 init table.
 *   5. display_set_backlight(1): backlight on.
 *   6. app_wake(): app-specific redraw (default no-op).
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

/* ── Sleep sentinel (SRAM, not zeroed on soft reset) ─────────────────
 * Written before entering Stop mode, cleared on genuine wake.
 * Survives an MCU soft reset (SRAM is only indeterminate on power-on).
 * Combined with IWDGRSTF check in main() to detect "IWDG fired during
 * deliberate Stop mode sleep" and silently re-enter Stop mode without
 * lighting the display.  Address is outside .bss — startup.s does not
 * zero it.  The PORRSTF guard in main() prevents false-positive on POR. */
#define SLEEP_SENTINEL_ADDR  ((volatile uint32_t *)0x20001FF8UL)
#define SLEEP_MAGIC          0xD0553E00UL

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

    /* ── Step 2: Settle delay — let the P-FET VCC-cut transient die out.
     *   Switching PA4/PA6 HIGH causes a voltage transient on the board that
     *   can couple onto PA7 and register as a spurious falling edge.  20 ms
     *   is enough for the power-rail glitch to settle before EXTI7 is armed.
     *   delay_ms feeds IWDG every tick so the watchdog stays happy here.   */
    delay_ms(20);

    /* ── Step 3: Enter MCU Stop mode (~10-20 µA) until genuine button press.
     *   system_enter_stop() arms EXTI7 (PA7, falling edge, EMR event mode),
     *   sets SLEEPDEEP, then sleeps via SEV+WFE+WFE.  IWDG continues on LSI.
     *
     *   IWDG timeout during sleep: the IWDG runs on LSI (~60 kHz) even in
     *   Stop mode and fires after ~17.5 s if not fed.  main() detects this via
     *   IWDGRSTF + SLEEP_SENTINEL_ADDR and silently re-enters Stop mode —
     *   the display stays dark and the user sees nothing.
     *
     *   Spurious-wake rejection: WFE can return on transients (VCC glitch,
     *   board noise) that briefly pull PA7 LOW without a real press.  A single
     *   end-of-delay sample is not enough — the transient may have already
     *   resolved, making PA7 look HIGH, OR it may coincidentally still be LOW.
     *
     *   Solution — continuous hold check: after WFE returns, sample PA7 every
     *   1 ms for 200 ms.  PA7 must stay LOW the entire time to be accepted as
     *   a genuine press.  If it goes HIGH at ANY point during the window, the
     *   wake was spurious and we immediately re-enter Stop mode.
     *   A real button press (user holds to wake the device) easily sustains
     *   LOW for 200 ms+.  Board transients are microseconds to low tens of ms.
     *   IWDG is fed every 1 ms in the hold-check loop; 26 s budget is ample.  */
    *SLEEP_SENTINEL_ADDR = SLEEP_MAGIC;   /* mark: entering Stop mode sleep    */
    while (1) {
        IWDG_FEED();
        system_enter_stop();

        /* Continuous hold check: PA7 must stay LOW for BTN_HOLD_WAKE_MS ms. */
        {
            uint8_t held = 1;
            uint16_t i;
            for (i = 0; i < BTN_HOLD_WAKE_MS; i++) {
                delay_ms(1);
                if (BTN_PORT->IDR & (1u << BTN_PIN)) { held = 0; break; }
            }
            if (held) break;   /* PA7 stayed LOW the whole window → genuine press */
        }
        /* PA7 went HIGH during the window — spurious wake, loop back to sleep */
    }

    *SLEEP_SENTINEL_ADDR = 0;             /* clear: genuine wake, no longer sleeping */

    /* ── Step 4: Wait for button release before re-initialising display.
     *   Genuine press confirmed.  Wait for release (PA7 HIGH) so the wake
     *   press is not registered as a game input on the first frame.          */
    while (!(BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); } /* await HIGH */

    /* ── Step 5: Restore VCC and reinitialize display.
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
    /* ── Step 0: Drive PA4/PA6 HIGH immediately — keep display dark ───────
     *   After ANY MCU reset, GPIO resets to INPUT (high-Z).  The display VCC
     *   P-FETs (gates on PA4, PA6) are floating → VCC rail may come up.
     *   Drive them HIGH before any other init to keep the display powered off
     *   regardless of why main() was entered (IWDG, NRST, POR, etc.).
     *   PA5 is NOT touched here — it is the coil gate, HIGH fires the element. */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;   /* enable GPIOA clock */
    {
        uint32_t m = GPIOA->MODER;
        m = (m & ~((3UL<<8)|(3UL<<12))) | ((1UL<<8)|(1UL<<12));  /* PA4,PA6 output */
        GPIOA->MODER = m;
    }
    GPIOA->BSRR = (1UL<<4)|(1UL<<6);      /* PA4=HIGH, PA6=HIGH — VCC off */

    /* ── Step 1: Save reset cause to SRAM (readable by OpenOCD) ───────────
     *   N32G031 RCC_CTRLSTS (0x40021024) reset flags — bits 3-8:
     *   bit3=PINRSTF  bit4=PORRSTF  bit5=SFTRSTF  bit6=IWDGRSTF
     *   bit7=WWDGRSTF bit8=LPWRRSTF
     *   Flags accumulate until RMRSTF bit is written.  Read via OpenOCD:
     *     mdw 0x20001FF0 2                                                 */
    uint32_t rst = RCC->CSR;               /* RCC_CTRLSTS */
    *((volatile uint32_t *)0x20001FF0UL) = rst;
    *((volatile uint32_t *)0x20001FF4UL) = 0xDEADBEEFUL;

    /* ── Step 2: Detect IWDG timeout during sleep — re-enter Stop silently ─
     *   The IWDG continues counting on LSI during Stop mode and fires after
     *   ~17.5 s (PR=6, RLR=4095, LSI≈60 kHz).  When it fires, the MCU resets
     *   and main() runs again.  Without this guard the display would reinit,
     *   the game would restart, and the user would see the device "wake itself."
     *
     *   Detection: IWDGRSTF (bit6) was set by hardware AND the sleep sentinel
     *   (written by device_sleep() before entering the Stop loop) is still
     *   present in SRAM.  PORRSTF (bit4) excluded — genuine power-on means
     *   SRAM content is indeterminate, so the sentinel is untrusted.
     *
     *   Recovery: minimal GPIO/TIM3 init only, re-enter the identical Stop
     *   loop used by device_sleep().  Display stays dark (PA4/PA6 already
     *   HIGH from step 0).  Falls through to full init when user wakes device. */
    if ((rst & (1UL<<6)) &&                    /* IWDGRSTF: IWDG fired        */
        (*SLEEP_SENTINEL_ADDR == SLEEP_MAGIC)) { /* was in intentional sleep  */
        /* Note: PORRSTF (bit4) is NOT checked here — reset flags accumulate
         * and PORRSTF stays set from first power-on until RMRSTF is written.
         * The sentinel magic (0xD0553E00) is a 1-in-4B false-positive risk
         * on POR with random SRAM — negligible and safe to ignore.          */

        IWDG_START();
        IWDG_CONFIGURE_26S();
        IWDG_FEED();
        vape_safety_init();

        /* PA7 button: input with pull-up (GPIOA clock enabled above) */
        {
            uint32_t m = GPIOA->MODER;
            m &= ~(3UL << (BTN_PIN * 2));
            GPIOA->MODER = m;
            uint32_t p = GPIOA->PUPDR;
            p = (p & ~(3UL<<(BTN_PIN*2))) | (1UL<<(BTN_PIN*2));
            GPIOA->PUPDR = p;
        }

        /* TIM3: required by delay_ms() inside system_enter_stop()'s hold check */
        RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
        TIM3->CR1 = 0; TIM3->PSC = 7; TIM3->ARR = 999;
        TIM3->EGR = TIM_EGR_UG; TIM3->SR = 0; TIM3->CR1 = TIM_CR1_CEN;

        /* Silent Stop mode loop (identical to device_sleep() inner loop) */
        while (1) {
            IWDG_FEED();
            system_enter_stop();
            {
                uint8_t held = 1;
                uint16_t i;
                for (i = 0; i < BTN_HOLD_WAKE_MS; i++) {
                    delay_ms(1);
                    if (BTN_PORT->IDR & (1u << BTN_PIN)) { held = 0; break; }
                }
                if (held) break;
            }
        }

        /* Genuine press confirmed: wait for release, clear sentinel */
        while (!(BTN_PORT->IDR & (1u << BTN_PIN))) { IWDG_FEED(); }
        *SLEEP_SENTINEL_ADDR = 0;
        /* Fall through to full normal init below */
    }

    /* ── Normal startup ───────────────────────────────────────────────── */
    IWDG_START();
    IWDG_CONFIGURE_26S();   /* extend timeout: default PR=0 gives ~410ms — too short for Stop-mode sleep */
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
