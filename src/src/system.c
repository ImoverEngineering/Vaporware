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

void clock_boost_48mhz(void) {
    /* Upgrade SYSCLK from 8 MHz HSI to 48 MHz (HSI × 6 via PLL).
     *
     * Must be called after clock_init() (TIM3 running at 1 MHz / PSC=7)
     * and before display_init() or any SPI use.
     *
     * Effects:
     *   • SYSCLK / APB2 → 48 MHz  (SPI1 BR_DIV2 → 24 MHz automatically)
     *   • TIM3 PSC updated 7 → 47  (keeps 1 MHz / 1 ms tick)
     *   • Flash: 1 wait-state set before the switch (required above 24 MHz)
     *
     * PLL maths: RCC_CFGR_PLLMULL_6 = (4UL<<18) → ×6 multiplier.
     * PLLSRC = 0 selects HSI direct (8 MHz, NOT HSI/2 on this device).
     * 8 MHz × 6 = 48 MHz — confirmed by n32g031.h comment. */

    /* 1. Flash: 2 wait states + prefetch — MUST come before the clock switch.
     * The N32G031 runs at 3.0 V (not 3.3 V); at this voltage the flash
     * needs 2 wait states above ~24 MHz to guarantee correct reads.
     * 2WS is safe at any frequency up to the device maximum. */
    FLASH_IF->ACR = (1UL << 4) | (2UL << 0);   /* PRFTBE | LATENCY_2WS */

    /* Sentinel log at 0x20000040..0x2000005C — readable via OpenOCD mdw.
     * Each slot is overwritten as execution progresses:
     *   [0x40] = 0xAA001111 → reached step 2 (PLL config)
     *   [0x44] = 0xBB002222 → PLLON asserted
     *   [0x48] = 0xCC003333 → PLLRDY confirmed  / 0xEE005555 if timed out
     *   [0x4C] = 0xDD004444 → SW write done
     *   [0x50] = CFGR after SW write + SWS wait
     *   [0x54] = CFGR2  [0x58] = CFGR3  [0x5C] = CR  */
    volatile uint32_t *dbg = (volatile uint32_t *)0x20000040UL;

    /* 2. Configure PLL: HSI (PLLSRC=0) × 6 (PLLMULL_6).
     * Set SW=PLL here, BEFORE enabling PLL (PLLON).
     * On N32G031 the hardware auto-switches SYSCLK to PLL the moment PLLRDY
     * asserts — writing SW after PLLON may miss the switch window.
     * SW=10 (0x2) is PLL selector (confirmed: SW=01 = HSE, rejects when HSE off). */
    dbg[0] = 0xAA001111UL;
    RCC->CFGR = (RCC->CFGR & ~((0xFUL << 18) | (1UL << 16) | 0x3UL))
              | RCC_CFGR_PLLMULL_6              /* PLLMULL_6 = ×6 */
              | RCC_CFGR_SW_PLL;               /* SW=10 set BEFORE PLLON */

    /* 3. Enable PLL and wait for lock + SYSCLK switch together.
     * N32G031 sets bit14 (RCC_CFGR_SWS_PLL) when PLL becomes SYSCLK —
     * the conventional SWS field at bits[3:2] never updates on this device.
     * bit14 is hardware-set on PLLRDY and hardware-cleared on SW revert. */
    dbg[1] = 0xBB002222UL;
    RCC->CR |= (1UL << 24);                     /* PLLON */
    {
        uint32_t i;
        for (i = 0; i < 200000UL; i++) {
            IWDG_FEED();
            if ((RCC->CR   & (1UL << 25)) &&        /* PLLRDY set */
                (RCC->CFGR & RCC_CFGR_SWS_PLL))     /* bit14: PLL is SYSCLK */
                break;
        }
        if (!(RCC->CR & (1UL << 25))) {
            dbg[2] = 0xEE005555UL;              /* PLLRDY TIMED OUT */
            FLASH_IF->ACR = (1UL << 4) | (0UL << 0);
            RCC->CFGR = (RCC->CFGR & ~0x3UL);  /* SW = 00 (HSI) */
            RCC->CR &= ~(1UL << 24);            /* PLLON = 0 */
            return;
        }
        dbg[2] = 0xCC003333UL;                  /* PLLRDY confirmed */
    }

    /* 4. Snapshot and check — bit14 should be set if PLL is SYSCLK. */
    dbg[3] = 0xDD004444UL;
    dbg[4] = RCC->CFGR;   /* CFGR: expect bit14=1, SW=10 */
    dbg[5] = RCC->CFGR2;  /* CFG2 — PREDIV / routing bits */
    dbg[6] = RCC->CFGR3;  /* CFG3 — EMCCTRL              */
    dbg[7] = RCC->CR;     /* CR   — PLLRDY, PLLON         */

    if (!(RCC->CFGR & RCC_CFGR_SWS_PLL)) {
        /* bit14 not set — PLL did not become SYSCLK; revert safely */
        RCC->CFGR = (RCC->CFGR & ~0x3UL);           /* SW = 00 (HSI) */
        FLASH_IF->ACR = (1UL << 4) | (0UL << 0);    /* PRFTBE, 0WS   */
        RCC->CR &= ~(1UL << 24);                     /* PLLON = 0     */
        return;
    }

    /* 5. Recalibrate TIM3: 48 MHz / (47+1) = 1 MHz → 1 ms tick unchanged */
    TIM3->CR1 = 0;
    TIM3->PSC = 47;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR  = 0;
    TIM3->CR1 = TIM_CR1_CEN;
}

void system_enter_stop(void) {
    /* Enter N32G031 Stop mode; return after button (PA7) falling edge.
     *
     * Stop mode: HSI and PLL halt, core draws ~10-20 µA.
     *   IWDG continues on LSI (~40 kHz) — feeds required before entry;
     *   timeout ~26 s (PR=6, RLR=4095).  IWDG is NOT fed inside this function.
     *
     * Wake source: EXTI7 falling edge (PA7, active-LOW button) in event mode.
     *   SEV + WFE clears any stale ARM event latch; second WFE enters Stop.
     *   EXTI_PR is cleared between the two WFEs to drain transients.
     *
     * CRITICAL post-wake step: on N32G031, APB1/APB2 peripheral clocks do NOT
     *   automatically restart after Stop mode exit.  HSI must be explicitly
     *   re-enabled and TIM3/TIM1 must be restarted before any call to
     *   delay_ms() or ms_now().  Omitting this causes TIM3 to stay stopped,
     *   delay_ms() to spin indefinitely (no IWDG feed in inner wait loop),
     *   and the IWDG to reset the MCU — appearing as a boot loop after sleep.
     *
     * Register map (confirmed via factory Ghidra analysis):
     *   AFIO_EXTICR2 = 0x4001000C  (bits[15:12] = port for EXTI7)
     *   EXTI_IMR  = 0x40010400   EXTI_EMR  = 0x40010404
     *   EXTI_RTSR = 0x40010408   EXTI_FTSR = 0x4001040C
     *   EXTI_PR   = 0x40010414
     *   PWR_CR    = 0x40007000   (PDDS bit1, LPDS bit0)
     *   SCR       = 0xE000ED10   (SLEEPDEEP bit2)                            */

    volatile uint32_t *AFIO_EXTICR2 = (volatile uint32_t *)0x4001000CUL;
    volatile uint32_t *EXTI_IMR     = (volatile uint32_t *)0x40010400UL;
    volatile uint32_t *EXTI_EMR     = (volatile uint32_t *)0x40010404UL;
    volatile uint32_t *EXTI_RTSR    = (volatile uint32_t *)0x40010408UL;
    volatile uint32_t *EXTI_FTSR    = (volatile uint32_t *)0x4001040CUL;
    volatile uint32_t *EXTI_PR      = (volatile uint32_t *)0x40010414UL;
    volatile uint32_t *PWR_CR       = (volatile uint32_t *)0x40007000UL;
    volatile uint32_t *SCR          = (volatile uint32_t *)0xE000ED10UL;

    /* 1. Enable AFIO clock (APB2ENR bit0) and PWR clock (APB1ENR bit28). */
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= (1UL << 28);

    /* 2. Route EXTI7 to GPIOA: AFIO_EXTICR2 bits[15:12] = 0000 (GPIOA). */
    *AFIO_EXTICR2 &= ~(0xFUL << 12);

    /* 3. EXTI7: event mode (EMR), falling edge, clear stale pending. */
    *EXTI_IMR  &= ~(1UL << 7);   /* disable interrupt mode */
    *EXTI_EMR   =  (1UL << 7);   /* event mode only (direct assign, no stale bits) */
    *EXTI_RTSR &= ~(1UL << 7);   /* no rising-edge */
    *EXTI_FTSR |=  (1UL << 7);   /* falling-edge trigger */
    *EXTI_PR    =  (1UL << 7);   /* clear any stale pending (W1C) */

    /* 4. PWR_CR: PDDS=0 (Stop) + LPDS=1 (low-power regulator). */
    *PWR_CR = (*PWR_CR & ~(1UL << 1)) | (1UL << 0);

    /* 5. SCR SLEEPDEEP (bit2). */
    *SCR |= (1UL << 2);

    /* 6. SEV + WFE clears the ARM event latch (first WFE returns immediately).
     *    Second EXTI_PR clear drains any edge that arrived during setup.
     *    Second WFE enters Stop mode and blocks until EXTI7 fires.           */
    __asm volatile ("sev");
    __asm volatile ("wfe");
    *EXTI_PR = (1UL << 7);
    __asm volatile ("wfe");

    /* ── Execution resumes here after EXTI7 event (PA7 falling edge) ── */

    /* 7. Clear SLEEPDEEP. */
    *SCR &= ~(1UL << 2);

    /* 8. Disarm EXTI7 event and clear pending. */
    *EXTI_EMR &= ~(1UL << 7);
    *EXTI_PR   =  (1UL << 7);

    /* 9. Re-enable HSI and wait for ready.
     *    N32G031 Stop mode halts HSI; it does not auto-restart on exit.
     *    APB1/APB2 clocks derive from HSI, so TIM3/TIM1 remain stopped until
     *    HSI is explicitly re-enabled here.                                   */
    RCC->CR |= RCC_CR_HSION;
    {
        uint32_t i;
        for (i = 0; i < 200000UL; i++) {
            if (RCC->CR & RCC_CR_HSIRDY) break;
        }
    }

    /* 10. Restart TIM3 at 8 MHz / (7+1) = 1 MHz → 1 ms tick.
     *     PSC/ARR registers are reset after Stop mode on N32G031.
     *     Must be re-written before delay_ms() is safe to call.              */
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    TIM3->CR1 = 0;
    TIM3->PSC = 7;
    TIM3->ARR = 999;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR  = 0;
    TIM3->CR1 = TIM_CR1_CEN;

    /* 11. Restart TIM1 at 8 MHz / 8000 = 1 kHz for ms_now(). */
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    TIM1->CR1 = 0;
    TIM1->PSC = 7999;
    TIM1->ARR = 0xFFFF;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR  = 0;
    TIM1->CR1 = TIM_CR1_CEN;
}

void tim1_init(void) {
    /* Enable TIM1 clock on APB2 (TIM1EN = APB2ENR bit 11) */
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    TIM1->CR1 = 0;

    /* Auto-detect SYSCLK so this function is correct whether called before or
     * after clock_boost_48mhz().
     *   HSI 8 MHz:  PSC = 7999  → 8 000 000 / 8000 = 1 kHz
     *   PLL 48 MHz: PSC = 47999 → 48 000 000 / 48000 = 1 kHz
     * RCC_CFGR_SWS_PLL = bit14 (N32G031): set when PLL is SYSCLK. */
    uint32_t psc = (RCC->CFGR & RCC_CFGR_SWS_PLL) ? 47999u : 7999u;
    TIM1->PSC = psc;
    TIM1->ARR = 0xFFFF;  /* Full 16-bit range: wraps after 65535 ms */

    /* UG: force-load PSC and ARR shadow registers; reset CNT.
     * Same reason as TIM3 — mandatory before first CEN. */
    TIM1->EGR = TIM_EGR_UG;

    /* Clear UIF set by UG before starting the counter */
    TIM1->SR  = 0;

    /* CEN: start free-running.  TIM1 is never stopped in normal operation. */
    TIM1->CR1 = TIM_CR1_CEN;
}
