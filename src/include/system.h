/* vaporware/include/system.h — Clock, timing, and IWDG API for N32G031
 *
 * Target: N32G031K8Q7-1, 8 MHz HSI (no PLL).
 *
 * WHY TIM3/TIM1 INSTEAD OF SYSTICK:
 *   SysTick is not functional on the N32G031 variant used in the Raz DC25000 —
 *   writes to SYST_CSR are silently discarded and the counter never runs.
 *   TIM3 and TIM1 are used instead for all timing.
 *
 * Two timers:
 *   TIM3 (APB1, base=0x40000400) — 1 MHz tick timebase for delay_ms().
 *     PSC=7 → 8MHz/(7+1)=1MHz; ARR=999 → UIF every 1ms.
 *     Polled (not interrupt-driven) in delay_ms().
 *     Feeds IWDG every 1ms — safe for the watchdog while blocking.
 *     Initialised by clock_init().
 *
 *   TIM1 (APB2, base=0x40012C00) — free-running 16-bit counter at 1 kHz.
 *     PSC=7999 → 8MHz/8000=1kHz; ARR=0xFFFF → wraps every 65535 ms (~65 s).
 *     Read non-blocking with ms_now(). Not used for interrupts or PWM.
 *     Initialised by tim1_init() — call after clock_init().
 *
 * WRAP WARNING — ms_now() / millis():
 *   Both functions return a value that wraps at 65535 ms (~65 seconds).
 *   Callers MUST use (uint16_t) subtraction for safe elapsed-time comparisons:
 *
 *     uint16_t start = ms_now();
 *     // ... later ...
 *     if ((uint16_t)(ms_now() - start) >= TIMEOUT_MS) { ... }
 *
 *   Plain uint32_t subtraction will give wrong results once TIM1 wraps.
 *   uint32_t millis() returns a 32-bit value but the hardware counter is
 *   only 16 bits wide — the upper bits are always zero.
 *
 * Startup sequence (app.c main() calls these in order):
 *   clock_init();      // HSI on, TIM3 started
 *   delay_ms(50);      // settle time before LCD init
 *   display_init();    // uses delay_ms internally (up to ~300 ms)
 *   tim1_init();       // TIM1 wall clock started; ms_now() usable from here
 */
#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include "n32g031.h"

/* Initialise 8 MHz HSI clock (no PLL) and TIM3 1ms polling timebase.
 * Must be called before delay_ms(), display_init(), or any other timing.
 * Also starts IWDG feeding via the TIM3 tick. */
void clock_init(void);

/* Block for exactly ms milliseconds.
 * Polls TIM3 UIF flag (1ms tick) and feeds IWDG every tick.
 * Safe to call at any point — will not trigger watchdog reset during sleep.
 * Maximum safe call: limited only by uint32_t range (~49 days). */
void delay_ms(uint32_t ms);

/* Returns current TIM1 counter value in milliseconds (0–65535).
 * Equivalent to (uint32_t)ms_now() — provided for compatibility.
 *
 * WARNING: wraps at 65535 ms. Use (uint16_t) subtraction for deltas. */
uint32_t millis(void);

/* Boost SYSCLK from 8 MHz HSI to 48 MHz via PLL (HSI × 6).
 * Call after clock_init() and before display_init().
 * Updates TIM3 prescaler so delay_ms() remains accurate.
 * SPI1 reaches 24 MHz automatically (BR_DIV2 of APB2=48MHz). */
void clock_boost_48mhz(void);

/* Initialise TIM1 as a free-running 1 kHz wall clock.
 * Call once after clock_init() and before the first call to ms_now().
 * TIM1 is not used for interrupts, capture, or PWM in this firmware. */
void tim1_init(void);

/* Read TIM1 counter in milliseconds. Range: 0–65535. Wraps every ~65 s.
 *
 * ALWAYS use (uint16_t) subtraction for elapsed-time comparisons:
 *   (uint16_t)(ms_now() - start_ms) >= timeout_ms
 *
 * Plain uint32_t or int comparisons will give wrong results after wrap. */
static inline uint16_t ms_now(void) {
    return (uint16_t)TIM1->CNT;
}

#endif /* SYSTEM_H */
