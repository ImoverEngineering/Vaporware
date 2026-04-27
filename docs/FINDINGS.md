# Raz DC25000 Reverse Engineering Findings

## Hardware

- **MCU**: Nations Tech N32G031K8Q7-1 — ARM Cortex-M0 @ 48MHz, 64KB flash, 8KB SRAM
- **Display**: 128x160 IPS TFT, GC9107 controller (panel: CDI F17RN1048-1 / same as GC9107-Vape-lcd-Arduino-Library)
- **External Flash**: GT25Q80A 8Mbit (1MB) SPI NOR Flash
- **MCU GPIO base addresses**: GPIOA=0x40010800, GPIOB=0x40010C00, GPIOC=0x40011000

## Confirmed GPIO Pin Assignments (from Ghidra analysis of fw_dump.bin)

### LCD (GC9107) — SPI1, Mode 0 (CPOL=0, CPHA=0)
| Pin | Function     | Confirmed by                                 |
|-----|--------------|----------------------------------------------|
| PB3 | LCD SCLK     | GPIO init at 0x3358: GPIOB bit3 cleared LOW  |
| PB5 | LCD MOSI     | GPIO init at 0x3374: GPIOB bit5 cleared LOW  |
| PA15| LCD CS (active-low) | GPIO init at 0x3392: GPIOA bit15 cleared LOW |
| PB7 | LCD D/C      | sendCmd (0x35A4): GPIOB bit7=0x80 cleared (cmd mode) |
|     |              | sendData (0x35C4): GPIOB bit7=0x80 set (data mode)   |
| PB6 | LCD RST (active-low) | GPIO init at 0x33CC/33F6/3406: RST pulse sequence |

### LCD RST Sequence (matches GC9107 init exactly)
- HIGH (initial) → LOW (100ms) → HIGH (120ms)

### External SPI Flash (GT25Q80A) — GPIOA bit-bang (SPI2 hardware NOT used)
Confirmed by scanning bit-shift constants in flash functions 0x7500–0x7800:
| Pin  | Function         | Evidence                                              |
|------|------------------|-------------------------------------------------------|
| PA8  | Flash CS (active-low) | `FF 20 01 30` → R0=0x100=bit8 @ 0x7540         |
| PA9  | Flash SCK        | `01 20 40 02` → LSLS R0,#9 → R0=0x200=bit9 @ 0x751A |
| PA10 | Flash MISO       | Input — pulled-up, alternates with MOSI in bit-bang loop |
| PA11 | Flash MOSI       | `01 20 C0 02` → LSLS R0,#11 → R0=0x800=bit11 @ 0x7530 |

Note: SPI2 peripheral (0x40002800) is absent from all literal pool scans — flash is definitely bit-banged.
- Flash memory map: see FLASH_MAP.md

### Other Peripherals (partially confirmed from Ghidra)
| Peripheral | Function            | Source              |
|------------|---------------------|---------------------|
| ADC1 (0x40012000) | Vape/mic detection | Referenced at 0x1D28, 0x2F2C, 0x34F8, 0x4BD8 |
| 0x40014400 | Unknown (COMP or ADC2) | Used alongside ADC1 in vape-detect path |
| TIM3 (0x40000400) | Timing/PWM | Referenced at 0x3814, 0x5028 |
| DAC  (0x40007000) | Possibly coil drive or backlight | Function at 0x42BC writes DAC_DHR12R1 |
| EXTI (0x40010400) | Edge-triggered vape detection | Referenced at 0x21C4, 0x21EC, 0x2274 |
| USART2 (0x40003000)| Debug/comms | Referenced at 0x2FAC–0x2FD8 |
| DMA1 (0x40020000) | SPI/ADC DMA | Referenced at 0x19A8, 0x5384 |

**SAFETY**: Coil pin NOT confirmed — DAC PA4/PA5 is the prime suspect.
Custom firmware leaves all non-LCD/non-flash GPIO pins as inputs (reset default), preventing inadvertent coil activation.

## Key Firmware Functions

| Address | Function                                   |
|---------|--------------------------------------------|
| 0x35A4  | sendCmd(byte) — DC LOW, SPI write          |
| 0x35C4  | sendData(byte) — DC HIGH, SPI write        |
| 0x2EF8  | GPIO clear (set pin LOW): (gpio_base, bitmask) |
| 0x2EFC  | GPIO set (set pin HIGH): (gpio_base, bitmask)  |
| 0x3278  | SPI send byte                              |
| 0x5DAC  | Post-SPI (CS restore)                     |
| 0x3344  | LCD hardware init (SPI + RST sequence)     |
| 0x27C0  | GC9107 init sequence start                 |
| 0x6794  | Delay (ms) function                        |
| 0x1930  | send_command_sequence (CASET/RASET/RAMWR)  |

## GC9107 Init Sequence
Starts at firmware address 0x27C0 with FF→A5 (unlock), then 3E→08, 3A→65 (COLMOD 16-bit RGB565), etc.
Full init sequence available in GC9107_Vape Arduino library (confirmed match via logic analyzer capture).

**SPI Mode**: CPOL=0, CPHA=0 (Mode 0), MSB-first, 8-bit words
**Display size**: 128×160 pixels, RGB565

## External Flash Memory Map
See FLASH_MAP.md (documented by ginbot86 and xbenkozx repos — applies to all RAZ/KRAZE 25000 variants).

**Critical offsets:**
- `0xF8000`: Vape timer (4 bytes, LE, unit = 0.01s)
- `0xF8004`: Vape-in-use flag (0xBB = initialized)
- Timer resets to 0 if flag ≠ 0xBB or all bytes = 0xFF
- All image slots: RGB565 raw bitmaps, stored at documented offsets (see flash_map.md)
