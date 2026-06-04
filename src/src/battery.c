/* vaporware/src/battery.c — Battery ADC driver for N32G031 vape hardware
 *
 * Hardware: PA6 = battery sense pin, ADC1 channel 6.
 *   ADC base: 0x40020800 (confirmed on Raz DC25000 via OpenOCD, 2026-04-09).
 *   VDDA: 3.0 V (LDO, not 3.3 V) — all raw-to-voltage conversions use 3.0 V.
 *   Divider ratio ≈ 0.71 (Vpin = Vbat × 0.71); confirmed by live ADC scan 2026-05-29.
 *
 * Voltage formula:  Vbat = raw × 1.41 × 3.0 / 4096
 * Use the threshold constants in config.h directly (BAT_FULL=3582, WARN=2906, CRIT=2422).
 *
 * bat_init()      — initialises ADC peripheral; does NOT change PA6 GPIO mode.
 * bat_read_raw()  — temporarily switches PA6 to analog, triggers one conversion,
 *                   restores the original GPIO mode, and returns the 12-bit result.
 *
 * ADC channel and battery thresholds are configured in config.h:
 *   BAT_ADC_CHANNEL=6, BAT_GPIO_PORT=GPIOA, BAT_GPIO_PIN=6, VDDA=3.0V.
 */
#include "battery.h"
#include "config.h"

#define ADC_BASE  VAPE_ADC_BASE
#define ADC_STS   (*(volatile uint32_t *)(ADC_BASE + 0x00)) /* status: EOC=bit1 */
#define ADC_CTRL2 (*(volatile uint32_t *)(ADC_BASE + 0x08)) /* control: ADON=bit0, SWSTRRCH=bit22 */
#define ADC_SMPR2 (*(volatile uint32_t *)(ADC_BASE + 0x10)) /* sample time ch0-9 (3 bits each) */
#define ADC_RSEQ1 (*(volatile uint32_t *)(ADC_BASE + 0x30)) /* regular sequence length */
#define ADC_RSEQ3 (*(volatile uint32_t *)(ADC_BASE + 0x38)) /* regular seq ch1 (channel number) */
#define ADC_DAT   (*(volatile uint32_t *)(ADC_BASE + 0x50)) /* conversion result, bits[11:0] */

void bat_init(void) {
    /* Enable ADC1 clock: AHBENR bit 12 (not APB2ENR — ADC1 is on AHB on this device) */
    *(volatile uint32_t *)0x40021014UL |= (1UL << 12);

    /* CFGR2: ADC prescaler — 0x00003804 selects a divider that keeps
     * the ADC input clock within spec at 8 MHz HSI */
    *(volatile uint32_t *)0x4002102CUL = 0x00003804UL;

    /* Enable GPIOA clock (PA6 is the battery sense pin / ADC channel 6) */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    /* NOTE: PA6 GPIO mode is NOT changed to analog here.
     * bat_read_raw() saves GPIOA->MODER, temporarily switches PA6 to analog
     * (MODER=11) for the ADC conversion, then restores the saved mode so PA6
     * returns to output-LOW (display VCC enable).  Keeping PA6 as output-LOW
     * between readings ensures the display P-FET stays on at all times except
     * during the brief (~21-84 µs) conversion window.                     */

    /* SMPR2: set bits[20:18]=0b111 for channel 6 → 239.5-cycle sample time.
     * The long sample time ensures the ~0.71 divider resistor network fully
     * charges the ADC sample capacitor before conversion starts.            */
    ADC_SMPR2 |= (7UL << 18);  /* ch6 = bits[20:18] = 239.5 cycles */

    /* RSEQ1: regular sequence length = 0 → 1 conversion */
    ADC_RSEQ1  = 0x00000000UL;

    /* RSEQ3: first (and only) conversion = BAT_ADC_CHANNEL (6 = PA6) */
    ADC_RSEQ3  = (uint32_t)BAT_ADC_CHANNEL;

    /* CTRL2: ADON (bit0) = 1 → power on ADC */
    ADC_CTRL2  = 0x00000001UL;

    /* CTRL2: EXTSEL[3:1]=bits[19:17]=7 (software trigger), EXTTRIG=bit20=1
     * Required to allow SWSTRRCH (bit22) to start a conversion */
    ADC_CTRL2 |= (7UL << 17) | (1UL << 20);
}

uint16_t bat_read_raw(void) {
    /* Save GPIOA MODER, then set PA6 (BAT_GPIO_PIN) to analog mode (MODER=11).
     * Analog mode disconnects the digital output driver and Schmitt trigger,
     * which is required for accurate ADC readings — floating digital inputs
     * inject noise into the sample.
     *
     * PA6 is normally output-LOW (display VCC enable).  The saved MODER
     * restores output-LOW on exit, so the display P-FET stays on for all but
     * the ~21-84 µs conversion window.  The 239.5-cycle sample time exceeds
     * the RC settling time of the battery divider by >100×, so the reading
     * is accurate even though PA6 was at 0 V immediately before. */
    uint32_t saved_moder = BAT_GPIO_PORT->MODER;
    BAT_GPIO_PORT->MODER |= (3UL << (BAT_GPIO_PIN * 2));  /* MODER=11 = analog */

    /* Trigger conversion via SWSTRRCH (CTRL2 bit 22) and poll EOC (STS bit 1).
     * At 48 MHz PLL the tight loop below runs at ~10 ns/iter; 20 000 iters =
     * ~200 µs — ample margin for the longest ADC conversion (239.5 + 12.5
     * cycles at any supported ADC clock).  IWDG is fed once before and after
     * rather than inside the loop: each IWDG_FEED() write is a slow peripheral
     * bus transaction that inflates per-iteration time and defeats the purpose
     * of counting iterations as a time proxy at PLL speeds. */
    IWDG_FEED();
    ADC_STS    = 0;
    ADC_CTRL2 |= (1UL << 22);  /* SWSTRRCH: start conversion */
    for (uint32_t i = 0; i < 20000u && !(ADC_STS & 2U); i++);
    IWDG_FEED();

    /* Read 12-bit result from data register (bits[11:0]) */
    uint16_t r = (uint16_t)(ADC_DAT & 0xFFFU);

    /* Restore PA6 GPIO mode (may be input, output, or AF depending on caller) */
    BAT_GPIO_PORT->MODER = saved_moder;

    /* raw < 50 (≈ 0.04 V) indicates the ADC timed out, the battery is
     * disconnected, or a hardware fault.  Return BAT_FULL as a safe default
     * so the application does not immediately trigger a low-battery shutdown. */
    if (r < 50u) r = BAT_FULL;
    return r;
}
