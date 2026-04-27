/* vaporware/src/vape.c — Framework hardware safety init
 *
 * Coil MOSFET gate pin: PB0 (confirmed by Ghidra analysis of fw_dump.bin).
 *   PB0 LOW  = MOSFET off = coil cold (safe default)
 *   PB0 HIGH = MOSFET on  = coil active (heating element energised)
 *
 * vape_safety_init() is called as the VERY FIRST thing in main(), before any
 * other hardware init, to ensure the coil is never accidentally energised during
 * GPIO reconfiguration.  It only enables GPIO clocks; it does NOT configure PB0
 * because PB0 also serves as the battery ADC input (channel 8) — its mode is
 * managed by battery.c (bat_read_raw() switches it to analog temporarily).
 *
 * FIRE TRIGGER:
 *   The vaporware framework does NOT fire the coil automatically.
 *   Each application (game/app) is responsible for detecting the vape trigger
 *   condition (e.g., score >= 10 in FlappyVape) and driving PB0 HIGH for the
 *   desired duration.  This separation keeps the framework hardware-agnostic
 *   and avoids accidental coil activation in apps that don't need it.
 *
 * NOTE: PB0 is shared between battery sense (ADC ch8) and coil gate.
 *   bat_read_raw() saves and restores MODER, so interleaving ADC reads with
 *   coil control is safe — just avoid holding PB0 HIGH during an ADC read.
 */
#include "vape.h"
#include "config.h"

void vape_safety_init(void) {
    /* Enable GPIOA and GPIOB clocks before any pin configuration.
     * Called before clock_init() — at this point we are still on the reset
     * clock (typically HSI at low frequency), but RCC writes work.
     * PB0 (coil gate) is left in its reset state (input, no pull) here;
     * app-specific code configures it when needed. */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
}

void vape_init(void) {
    /* No-op in this SDK release.
     * Reserved for future coil PWM or temperature-sensing init. */
}
