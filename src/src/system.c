/* vaporware/src/system.c — Clock, TIM3 timebase, TIM1 wall clock for N32G031
 *
 * SysTick is NOT used — unimplemented on the N32G031K8Q7-1 used in the
 * Raz DC25000.  Writes to SYST_CSR are silently discarded and the counter
 * never increments.  TIM3 and TIM1 are used for all timing instead.
 *
 * TIM3 (APB1, base=0x40000400, TIM3EN=APB1ENR bit 1):
 *   PSC=7  → 8 MHz HSI / (7+1) = 1 MHz tick
 *   ARR=999 → UIF (update interrupt flag) fires every 1000 ticks = 1 ms
 *   Polled in delay_ms(); IWDG is fed each 1 ms tick.
 *   Not used for PWM or backlight — PB4 backlight is plain GPIO (active-LOW).
 *
 * TIM1 (APB2, base=0x40012C00, TIM1EN=APB2ENR bit 11):
 *   Free-running 16-bit counter at 1 kHz (wraps every 65535 ms ≈ 65 s).
 *   PSC=7999 → 8 MHz / 8000 = 1 kHz; ARR=0xFFFF (full 16-bit range).
 *   Read with ms_now() for non-blocking elapsed-time checks.
 *   Not used for interrupts, PWM, or capture.
 *   Callers must use (uint16_t) subtraction for safe delta comparisons.
 */
#include "system.h"
#include "n32g031.h"

static volatile uint32_t g_tick_ms = 0;

void SysTick_Handler(void) {} /* Satisfies weak alias in startup.s — SysTick is inert */

void clock_init(void) {
    /* Enable and wait for HSI (8 MHz internal RC oscillator).
     * No PLL: this firmware runs at 8 MHz to keep power and EMI low. */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    /* Enable prefetch buffer (PRFTBE, bit4) with 0 wait states.
     * At 8 MHz and VDDA=3.0V, 0WS is within spec (flash rated to 24 MHz at 3V). */
    FLASH_IF->ACR = (1UL << 4);   /* PRFTBE only; LATENCY_0WS = 0 */

    /* Enable TIM3 clock on APB1 (TIM3EN = APB1ENR bit 1) */
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;

    TIM3->CR1 = 0;           /* Stop timer and clear config before setup */
    TIM3->PSC = 7;           /* Prescaler: 8 MHz / (7+1) = 1 MHz */
    TIM3->ARR = 999;         /* Auto-reload: 1000 ticks = 1 ms */

    /* Write UG (update generation, EGR bit0) to force the PSC and ARR
     * shadow registers to load immediately and reset CNT to 0.
     * Without UG, PSC and ARR don't take effect until the next overflow. */
    TIM3->EGR = TIM_EGR_UG;

    /* Clear UIF: the UG event sets UIF as if a real overflow occurred.
     * Must be cleared here, otherwise the first poll in delay_ms() returns
     * immediately without waiting for a real 1 ms tick. */
    TIM3->SR  = 0;

    TIM3->CR1 = TIM_CR1_CEN; /* CEN: start the counter */
}

void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        /* Wait for TIM3 UIF (update interrupt flag = 1ms tick) */
        while (!(TIM3->SR & TIM_SR_UIF));
        /* Clear UIF by writing 0 to the flag bit (not the whole register) */
        TIM3->SR = 0;
        g_tick_ms++;
        /* Feed IWDG each millisecond — delay_ms() is safe for the watchdog
         * regardless of how long the delay is. */
        IWDG_FEED();
    }
}

uint32_t millis(void) {
    /* Read TIM1 counter directly — works anywhere, not just inside delay_ms().
     * Returns a 32-bit value but TIM1 is 16-bit; upper 16 bits are always 0.
     * Wraps at 65535 ms. Use (uint16_t) subtraction for correct delta math. */
    return (uint32_t)TIM1->CNT;
}

void tim1_init(void) {
    /* Enable TIM1 clock on APB2 (TIM1EN = APB2ENR bit 11) */
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    TIM1->CR1 = 0;
    TIM1->PSC = 7999;    /* 8 MHz HSI / (7999+1) = 1 kHz → 1 count per ms */
    TIM1->ARR = 0xFFFF;  /* Full 16-bit range: wraps after 65535 ms */

    /* UG: force-load PSC and ARR shadow registers; reset CNT.
     * Same reason as TIM3 — mandatory before first CEN. */
    TIM1->EGR = TIM_EGR_UG;

    /* Clear UIF set by UG before starting the counter */
    TIM1->SR  = 0;

    /* CEN: start free-running.  TIM1 is never stopped in normal operation. */
    TIM1->CR1 = TIM_CR1_CEN;
}
