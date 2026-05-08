# Vaporware — Embedded App Framework for Raz DC25000

Vaporware is a minimal C firmware framework for building games and apps on
the Raz DC25000 disposable vape device, which runs an N32G031K8Q7-1
microcontroller driving a 128×160 GC9107 LCD.

---

## Hardware Requirements

| Item | Value |
|---|---|
| MCU | N32G031K8Q7-1 (Cortex-M0, 8 MHz HSI; PLL can reach 48 MHz — see `clock_boost_48mhz()`) |
| Flash | 64 KB (top 4 KB reserved for NV storage) |
| SRAM | 8 KB |
| VDDA | 3.0 V (LDO-regulated, NOT 3.3 V — affects all ADC readings) |
| Display | GC9107, 128×160, RGB565, SPI1 @ 4 MHz |
| COLMOD | 0x65 (GC9107-specific 16-bit mode — not the standard 0x55) |

### Confirmed Pin Assignments

| Signal | Pin | Notes |
|---|---|---|
| SPI1_SCK | PB3 | AF0 |
| SPI1_MOSI | PB5 | AF0 |
| LCD CS | PA15 | Software GPIO, active-LOW |
| LCD D/C | PB7 | LOW=command, HIGH=data |
| LCD RST | PB6 | Active-LOW pulse at init |
| LCD Backlight | PB4 | Active-LOW GPIO (LOW=on, HIGH=off). NOT PWM. |
| Button | PA7 | Active-LOW, internal pull-up |
| Battery ADC | PB0 | ADC1 channel 8 |
| Coil gate | PB0 | Confirmed from fw_dump.bin Ghidra analysis. PB0 HIGH = coil on. |
| SWD SWDIO | PA13 | Fixed Cortex-M0 SWD pin |
| SWD SWCLK | PA14 | Fixed Cortex-M0 SWD pin |

> **PB0 is shared**: it serves as both the battery ADC input (channel 8) and
> the coil MOSFET gate. `bat_read_raw()` saves/restores MODER around ADC reads,
> so interleaving ADC reads with coil control is safe. Avoid driving PB0 HIGH
> during an ADC conversion.

---

## Peripheral Base Addresses

These are N32G031-specific and differ from STM32 values.

| Peripheral | Base Address | Enable bit |
|---|---|---|
| GPIOA | 0x40010800 | APB2ENR bit 2 |
| GPIOB | 0x40010C00 | APB2ENR bit 3 |
| SPI1 | 0x40012000 | APB2ENR bit 9 (NOT bit 12 like STM32F1) |
| TIM1 | 0x40012C00 | APB2ENR bit 12 |
| TIM3 | 0x40000400 | APB1ENR bit 1 |
| ADC1 | 0x40020800 | AHBENR bit 12 (NOT APB2ENR) |
| FLASH | 0x40022000 | — |
| IWDG | 0x40003000 | — |
| RCC | 0x40021000 | — |

---

## Module Descriptions

### `system` — Clock and Timing

Initialises 8 MHz HSI and provides millisecond timing via TIM3 (delay) and
TIM1 (wall clock). SysTick is not functional on this device variant.

- `clock_init()` — enable HSI, start TIM3 1ms timebase
- `tim1_init()` — start TIM1 free-running 1kHz counter
- `delay_ms(ms)` — blocking delay; feeds IWDG every 1ms
- `ms_now()` — read TIM1 in ms (0–65535, wraps every ~65 s)
- `millis()` — same as ms_now(), returns uint32_t
- `clock_boost_48mhz()` — switch from 8 MHz HSI to 48 MHz PLL (HSI×6). Sets 2WS
  flash latency before enabling PLL. Must be called **after** `display_init()` because
  the display init sequence is tested at 8 MHz. After boosting, update SPI prescaler:
  ```c
  clock_boost_48mhz();
  if (RCC->CFGR & RCC_CFGR_SWS_PLL) {
      // 48 MHz / 4 = 12 MHz SPI (GC9107 max ≈ 16 MHz)
      SPI1->CR1 = (SPI1->CR1 & ~(7UL << 3)) | SPI_CR1_BR_DIV4;
  }
  ```
  Used by the streamer to cut per-chunk blit time from ~17 ms (4 MHz) to ~2.7 ms
  (12 MHz), enabling ~7 fps streaming. Do **not** call `bat_init()` after boosting —
  it writes RCC_CFGR2 and can disturb the PLL.

**CRITICAL**: `ms_now()` wraps at 65535 ms. Always use `(uint16_t)` subtraction
for elapsed-time comparisons:
```c
uint16_t start = ms_now();
// ... later ...
if ((uint16_t)(ms_now() - start) >= TIMEOUT_MS) { ... }
```

### `display` — GC9107 LCD Driver

Initialises SPI1 and the GC9107 panel. Color format is RGB565 with BGR swap
(MADCTL=0x98, BGR=1): to display visual (R,G,B), send `((B>>3)<<11)|((G>>2)<<5)|(R>>3)`.
Use the `COL_RGB(r,g,b)` macro from display.h for convenience.

Key behaviors:
- CS stays LOW for entire window/fill operations (GC9107 state machine requirement)
- `display_set_window()` leaves CS LOW — caller must manage CS
- First `display_fill()` in `display_init()` flushes uninitialised GRAM
- Backlight (PB4) is active-LOW plain GPIO — no PWM
- **SPI OVR deadlock**: The N32G031 SPI OVR flag accumulates across separate transfers
  and permanently stalls TXE after 2–3 full-screen operations. Fixed by toggling
  `SPE=0` then `SPE=1` before each `display_fill()` or `display_draw_chunk_2x()` call
  to clear all SPI status flags (OVR, RXNE, etc.)

Additional display functions (not in the `app` framework path):
- `display_draw_chunk_2x(buf, row0, w, h)` — blit a 64×40 logical-pixel buffer to
  GRAM as a 128×80 physical block by doubling each pixel 2× horizontally and 2×
  vertically. Used by the streamer for 2× upscaling without host-side resize.
  `buf` is an array of `w×h` RGB565 words; `row0` is the first physical row (0 or 80).
- `display_sleep_in()` — send SLPIN command and turn off backlight (power save).

### `battery` — ADC Battery Monitor

Reads PB0 (ADC channel 8) for battery voltage. `bat_read_raw()` temporarily
switches PB0 to analog mode, triggers one conversion, and restores MODER.

Voltage formula (empirical divider ~0.71):
```
Vbat = raw * 1.41 * 3.0 / 4096
```

Use the threshold constants from `config.h` directly:
- `BAT_FULL` = 181 (≈ 3.70 V)
- `BAT_WARN` = 146 (≈ 3.00 V)
- `BAT_CRIT` = 122 (≈ 2.50 V)

ADC sample time is 239.5 cycles (longest available) because the ~96 kΩ
Thevenin source impedance of the divider limits ADC capacitor charging.

### `button` — PA7 Button Driver

Active-LOW, internal pull-up. No software debounce — the ~33ms frame rate
acts as natural debounce (mechanical bounce < 10ms).

- `button_update()` — call once per frame (app framework does this automatically)
- `button_pressed()` — 1 if held, 0 if not
- `button_just_pressed()` — 1 on the frame the button goes down (one-shot)
- `button_just_released()` — 1 on the frame the button is released (one-shot)
- `button_held_ms()` — ms continuously held; saturates at 65535 ms
- `button_raw()` — direct GPIO read, no state update; use this inside long render loops where `button_update()` is not being called

### `nv` — Write-Forward NV Flash Storage

8 keys × 512-byte flash pages at `0x0800F000–0x0800FFFF`. Each page holds
64 slots; a new write appends to the first blank slot without erasing.
Page is only erased when all 64 slots are full (~640,000 writes per key).

- `nv_read(key, default)` — returns stored value or default if blank
- `nv_write(key, val)` — appends to page; may erase if full (~30 ms)
- `nv_reset(key)` — erases the page for that key

See `include/nv.h` for the full key list (`NV_KEY_PUFF_COUNT`, `NV_KEY_HIGH_SCORE`, etc.)

### `vape` — Coil Safety Init

`vape_safety_init()` is called before `clock_init()` to enable GPIO clocks.
The coil gate (PB0) is not explicitly driven here — `bat_read_raw()` manages
PB0 mode. Apps that fire the coil must configure PB0 as output and drive it HIGH.

`vape_init()` is a no-op placeholder for future coil PWM or temp-sensing init.

### `app` — Application Framework

Provides `main()`. Apps implement `app_init()` and `app_update()`. Frame rate
is ~30 fps (APP_FRAME_MS=33). The framework handles: IWDG, hardware init order,
frame timing, sleep, button debounce, and hold-to-reset.

---

## How to Create a New App

1. Create a source file (e.g., `src/myapp.c`)
2. Include the framework headers
3. Implement the two required functions:

```c
#include "app.h"
#include "display.h"
#include "button.h"
#include "nv.h"

void app_init(void) {
    app_set_sleep_timeout(15000);           // sleep after 15s idle (0 = disable)
    app_set_hold_reset(10000, my_reset_fn); // 10s hold fires callback
    display_fill(COL_BLACK);               // initial draw
}

void app_update(uint32_t frame) {
    if (button_just_pressed()) {
        // respond to button press
    }
}

// Optional: called after waking from sleep
void app_wake(void) {
    display_fill(COL_BLACK); // full redraw after wake
}
```

4. Link against the vaporware object files: `system.o display.o button.o battery.o vape.o nv.o app.o`
5. Do NOT provide your own `main()` — the framework supplies it.

---

## Porting to a New Board

Edit only `include/config.h`. All other files derive pin assignments from that file.

Key settings to change:

| Define | Description |
|---|---|
| `BTN_PORT` / `BTN_PIN` | Button GPIO |
| `LCD_*_PORT` / `LCD_*_PIN` | Display SPI and control pins |
| `LCD_BL_PORT` / `LCD_BL_PIN` | Backlight GPIO (verify active-LOW assumption) |
| `BAT_ADC_CHANNEL` / `BAT_GPIO_PIN` | Battery sense ADC |
| `BAT_FULL` / `BAT_WARN` / `BAT_CRIT` | Threshold raw ADC values |
| `APP_FRAME_MS` | Target frame period in ms (default 33 = ~30fps) |
| `SYS_CLOCK_HZ` | Used for documentation; update TIM3/TIM1 prescalers in system.c |

If the clock changes from 8 MHz, update in `system.c`:
- `TIM3->PSC` for 1MHz tick: `(SYS_CLOCK_HZ / 1000000) - 1`
- `TIM1->PSC` for 1kHz tick: `(SYS_CLOCK_HZ / 1000) - 1`

---

## Build Workflow

```bash
# From project root (adjust paths to match your toolchain)
arm-none-eabi-gcc -mcpu=cortex-m0 -mthumb -Os \
    -Ivaporware/include \
    vaporware/src/system.c vaporware/src/display.c \
    vaporware/src/button.c vaporware/src/battery.c \
    vaporware/src/vape.c vaporware/src/nv.c vaporware/src/app.c \
    your_app/src/myapp.c \
    -T vaporware/n32g031.ld \
    -o myapp.elf

arm-none-eabi-objcopy -O binary myapp.elf myapp.bin
```

Flash via OpenOCD + hla_swd (see flash workflow memory note for trampoline details).

---

## Known Quirks

| Quirk | Detail |
|---|---|
| **SysTick absent** | Writes to SYST_CSR are silently discarded on this N32G031 variant. Use TIM3/TIM1 instead. Never use CMSIS `SysTick_Config()`. |
| **ms_now() wraps at 65535** | TIM1 is a 16-bit counter. Always use `(uint16_t)(ms_now() - start)` for elapsed time. |
| **SPI1EN at bit 9** | APB2ENR bit 9. Not bit 12 — that is TIM1EN (see below). |
| **TIM1EN at bit 12** | APB2ENR bit 12, NOT bit 11 as on STM32F0/F1. Confirmed by live OpenOCD register scan on Raz DC25000. Using bit 11 leaves TIM1 clocks off → `ms_now()` always returns 0 → all frame timing broken. |
| **VDDA = 3.0 V** | The LDO outputs 3.0 V, not 3.3 V. All ADC voltage calculations must use 3.0 V. |
| **ADC base = 0x40020800** | Not the common 0x40012400. Confirmed by live OpenOCD register reads. |
| **CS must stay LOW** | GC9107 requires CS to be held LOW for the entire multi-command init sequence and for each fill/window operation. Do not deassert CS between commands in a single transfer. |
| **BGR swap** | MADCTL=0x98 sets BGR=1. Use `COL_RGB(r,g,b)` macro — do not pass raw RGB565. |
| **Flash writes are 32-bit** | The N32G031 does not support half-word (16-bit) flash programming. Always write 32-bit aligned words. |
| **IWDG during sleep** | The IWDG is fed inside every busy-wait loop (flash, ADC, sleep polling). The watchdog never resets the device in normal operation. |
| **SPI OVR accumulates** | The SPI OVR (overrun) flag is set whenever a received byte is not read, and it is not automatically cleared between transfers. After 2–3 full-screen fills the flag permanently stalls TXE. Always toggle `SPE=0→1` before starting a new SPI burst. |
| **PLL disturbed by bat_init()** | `bat_init()` writes `RCC_CFGR2`, which touches the PLL divider field on some N32G031 revisions and causes the PLL to drop lock silently. Do not call `bat_init()` after `clock_boost_48mhz()`. |
| **SWD still works at 48 MHz** | OpenOCD briefly loses comms during the PLL switch but auto-recovers. Use `adapter speed 240` or lower in your OpenOCD config for reliable access at 48 MHz. |
