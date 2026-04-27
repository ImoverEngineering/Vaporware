/* vaporware/src/battery.c — Battery ADC driver for N32G031 vape hardware
 *
 * Hardware: PB0 = battery sense pin, ADC1 channel 8.
 *   ADC base: 0x40020800 (confirmed on Raz DC25000 via OpenOCD, 2026-04-09).
 *   VDDA: 3.0 V (LDO, not 3.3 V) — all raw-to-voltage conversions use 3.0 V.
 *
 * Voltage formula (empirical divider ratio ~0.71, not the 1:28 in config.h):
 *   Vbat = raw * 1.41 * 3.0 / 4096
 * Use the threshold constants in config.h directly; do not recompute from DIVIDER.
 *
 * bat_init()      — initialises ADC peripheral; does NOT change PB0 GPIO mode.
 * bat_read_raw()  — temporarily switches PB0 to analog, triggers one conversion,
 *                   restores the original GPIO mode, and returns the 12-bit result.
 *
 * ADC channel and battery thresholds are configured in config.h:
 *   BAT_ADC_CHANNEL=8, BAT_GPIO_PIN=0 (PB0), VDDA=3.0V, BAT_FULL/WARN/CRIT.
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

    /* Enable GPIOB clock (PB0 is the battery sense pin / ADC channel 8) */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    /* NOTE: PB0 GPIO mode is NOT changed to analog here.
     * bat_read_raw() temporarily switches the pin to MODER=11 (analog/high-Z)
     * for the duration of the conversion, then restores the saved mode.
     * This prevents the pin from floating between readings.               */

    /* SMPR2: set bits[26:24]=0b111 for channel 8 → 239.5-cycle sample time.
     * The long sample time is required because the ~96 kΩ Thevenin resistance
     * of the battery-sense resistor divider limits the charging current into
     * the ADC sample capacitor; a short sample time would give low results. */
    ADC_SMPR2 |= (7UL << 24);  /* ch8 = bits[26:24] = 239.5 cycles */

    /* RSEQ1: regular sequence length = 0 → 1 conversion */
    ADC_RSEQ1  = 0x00000000UL;

    /* RSEQ3: first (and only) conversion = BAT_ADC_CHANNEL (8 = PB0) */
    ADC_RSEQ3  = (uint32_t)BAT_ADC_CHANNEL;

    /* CTRL2: ADON (bit0) = 1 → power on ADC */
    ADC_CTRL2  = 0x00000001UL;

    /* CTRL2: EXTSEL[3:1]=bits[19:17]=7 (software trigger), EXTTRIG=bit20=1
     * Required to allow SWSTRRCH (bit22) to start a conversion */
    ADC_CTRL2 |= (7UL << 17) | (1UL << 20);
}

uint16_t bat_read_raw(void) {
    /* Save GPIOB MODER, then set PB0 (BAT_GPIO_PIN) to analog mode (MODER=11).
     * Analog mode disconnects the digital output driver and Schmitt trigger,
     * which is required for accurate ADC readings — floating digital inputs
     * can inject noise into the sample. */
    uint32_t saved_moder = GPIOB->MODER;
    GPIOB->MODER |= (3UL << (BAT_GPIO_PIN * 2));  /* MODER=11 = analog */

    /* Clear EOC flag, then trigger conversion via SWSTRRCH (CTRL2 bit 22).
     * SWSTRRCH = Software Start Regular Channel conversion.
     * The for-loop feeds IWDG while waiting for EOC (bit1 of STS).
     * 500 iterations at 8MHz is generous; conversion typically completes
     * in ~30 µs at 239.5-cycle sample time + 12.5 conversion cycles. */
    ADC_STS    = 0;
    ADC_CTRL2 |= (1UL << 22);  /* SWSTRRCH: start conversion */
    for (uint32_t i = 0; i < 500u && !(ADC_STS & 2U); i++)
        IWDG_FEED();

    /* Read 12-bit result from data register (bits[11:0]) */
    uint16_t r = (uint16_t)(ADC_DAT & 0xFFFU);

    /* Restore PB0 GPIO mode (may be input, output, or AF depending on caller) */
    GPIOB->MODER = saved_moder;

    /* raw < 50 (≈ 0.04 V) indicates the ADC timed out, the battery is
     * disconnected, or a hardware fault.  Return BAT_FULL as a safe default
     * so the application does not immediately trigger a low-battery shutdown. */
    if (r < 50u) r = BAT_FULL;
    return r;
}
