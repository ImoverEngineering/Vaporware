/* vaporware/include/config.h — Board hardware configuration
 *
 * This is the ONLY file you should need to edit when porting to a new
 * board revision.  Every other SDK file derives its pin assignments,
 * memory layout, and hardware constants from the definitions here.
 *
 * Confirmed for: GV2024 V1 and V8, Raz DC25000 (N32G031K8Q7-1)
 * MCU:  N32G031K8Q7-1, Cortex-M0, 8 MHz HSI, 64 KB flash, 8 KB SRAM
 * VDDA: 3.0 V (LDO-regulated, NOT 3.3 V — affects all ADC readings)
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "n32g031.h"

/* ── System clock ─────────────────────────────────────────────────────
 * HSI runs at 8 MHz with no PLL.  Changing this requires updating the
 * TIM3 (PSC=7, ARR=999) and TIM1 (PSC=7999) prescalers in system.c.  */
#define SYS_CLOCK_HZ    8000000UL

/* ── Button ───────────────────────────────────────────────────────────
 * Active-LOW: pin reads 0 when pressed.  Internal pull-up enabled.   */
#define BTN_PORT        GPIOA
#define BTN_PIN         7
#define BTN_RCC_EN      RCC_APB2ENR_IOPAEN

/* ── GC9107 display, SPI1 ─────────────────────────────────────────────
 * SPI data lines (SCK/MOSI) are AF0 on SPI1; all others are soft GPIO.
 * Backlight enable is active-LOW (LOW = on, HIGH = off).              */
#define LCD_SCK_PORT    GPIOB   /* PB3  SPI1_SCK  AF0 */
#define LCD_SCK_PIN     3
#define LCD_MOSI_PORT   GPIOB   /* PB5  SPI1_MOSI AF0 */
#define LCD_MOSI_PIN    5
#define LCD_CS_PORT     GPIOA   /* PA15 CS  (software, active-low)     */
#define LCD_CS_PIN      15
#define LCD_DC_PORT     GPIOB   /* PB7  D/C (LOW=cmd, HIGH=data)       */
#define LCD_DC_PIN      7
#define LCD_RST_PORT    GPIOB   /* PB6  RST (active-low pulse at init)  */
#define LCD_RST_PIN     6
#define LCD_BL_PORT     GPIOB   /* PB4  Backlight enable (active-LOW)   */
#define LCD_BL_PIN      4

/* ── Battery ADC ──────────────────────────────────────────────────────
 * bat_read_raw() temporarily switches the battery sense pin to analog
 * mode for the ADC conversion, then restores the previous GPIO mode.
 *
 * Thresholds (12-bit, VDDA=3.0V, ~1:28 resistor divider):
 *   Raw 205 ≈ 4.20 V (full)     Raw 181 ≈ 3.70 V (charged)
 *   Raw 146 ≈ 3.00 V (warn)     Raw 122 ≈ 2.50 V (critical)
 *
 * To re-derive thresholds:  raw = Vbat / (divider_ratio * VDDA) * 4096
 * Example: 3.7V / (28 * 3.0V) * 4096 ≈ 181                          */
#define BAT_ADC_CHANNEL 8       /* ADC channel for battery sense pin    */
#define BAT_GPIO_PIN    0       /* GPIO pin number for battery sense     */
#define BAT_FULL        181     /* ≈ 3.70 V — display full indicator     */
#define BAT_WARN        146     /* ≈ 3.00 V — display low indicator      */
#define BAT_CRIT        122     /* ≈ 2.50 V — force sleep immediately    */

/* ── NV flash storage ─────────────────────────────────────────────────
 * 8 keys × 512-byte pages, write-forward within each page.
 * The linker script caps code at 60 KB, leaving the top 4 KB
 * (0x0800F000–0x0800FFFF) permanently reserved for NV data.
 * Do NOT change NV_FLASH_BASE without also editing n32g031.ld.        */
#define NV_FLASH_BASE   0x0800F000UL
#define NV_NUM_KEYS     8

/* ── App framework ────────────────────────────────────────────────────
 * FRAME_MS controls the target frame period (~30 fps at 33 ms).      */
#define APP_FRAME_MS    33U

/* ── SWD debug pins (Cortex-M0 fixed, do not reassign) ───────────────
 * PA13 = SWDIO, PA14 = SWCLK.  Restored during sleep so the debugger
 * stays reachable even when all other GPIO are high-Z.                */
#define SWD_SWDIO_PIN   13
#define SWD_SWCLK_PIN   14

#endif /* CONFIG_H */
