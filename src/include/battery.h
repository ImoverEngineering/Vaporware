/* vaporware/include/battery.h — Battery ADC and charge-level API for N32G031
 *
 * ADC channel and voltage thresholds are set in config.h.
 * Default: ADC channel 8, VDDA=3.0V, resistor divider ~1:28.
 *
 * bat_read_raw() saves and restores the battery sense pin's GPIO mode
 * around each conversion so it is safe to call at any time.
 *
 * Usage:
 *   bat_init();                          // call once, from app_init()
 *   uint16_t raw = bat_read_raw();       // ~50 µs per call
 *   uint8_t  lvl = bat_level(raw);       // 0–3
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include "config.h"

/* Charge thresholds — defined in config.h with derivation notes:
 *   BAT_FULL  181   raw >= 181 → ≥3.70 V → 3 bars (full)
 *   BAT_WARN  146   raw >= 146 → ≥3.00 V → 2 bars (normal)
 *   BAT_CRIT  122   raw >= 122 → ≥2.50 V → 1 bar  (low)
 *             <122              → <2.50 V → 0 bars (critical — force sleep) */

/* Initialise ADC for battery reads.  Does NOT change PB0 GPIO mode.
 * Call once before bat_read_raw().                                    */
void bat_init(void);

/* Trigger one ADC conversion and return the 12-bit result.
 * Returns BAT_FULL on timeout or hardware error.                      */
uint16_t bat_read_raw(void);

/* Convert a raw reading to a 0–3 charge bar level. */
static inline uint8_t bat_level(uint16_t raw) {
    if (raw >= (uint16_t)BAT_FULL) return 3u;
    if (raw >= (uint16_t)BAT_WARN) return 2u;
    if (raw >= (uint16_t)BAT_CRIT) return 1u;
    return 0u;
}

#endif /* BATTERY_H */
