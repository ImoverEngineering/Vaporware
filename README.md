# Vaporware

A minimal C firmware SDK for building games and apps on the **Raz DC25000** disposable vape — repurposed as a pocket game console.

The device runs a Nations Tech **N32G031K8Q7-1** (ARM Cortex-M0) driving a 128×160 GC9107 IPS display, with a single button, battery ADC, and a coil MOSFET you can optionally fire. Total cost: ~$15. Total fun: outsized.

---

## Disclaimer

> **This project is for educational and research purposes only.**
>
> By using this project you acknowledge that:
>
> - **You assume all risk.** Modifying consumer electronics — especially lithium battery-powered devices — carries real hazards including fire, explosion, electric shock, and permanent hardware damage. Proceed only if you understand what you are doing.
> - **The coil is dangerous.** The heating coil draws significant current from the LiPo cell. Custom firmware that fires the coil incorrectly (wrong duty cycle, no thermal cutoff, wrong timing) can cause the battery to overheat, vent, or catch fire. The examples in this repo do **not** fire the coil. If you choose to, you do so entirely at your own risk.
> - **Vaping carries health risks.** This project does not encourage or endorse vaping. The hardware is used purely as a convenient, cheap embedded platform.
> - **No warranty is provided.** The authors are not responsible for any damage to property, injury to persons, or any other consequence arising from the use of this software or the techniques described here.
> - **Respect local laws.** Possession and modification of vaping devices may be regulated or prohibited in your jurisdiction.

---

## Hardware at a Glance

| | |
|---|---|
| **MCU** | N32G031K8Q7-1, Cortex-M0 @ 8 MHz, 64 KB flash, 8 KB SRAM |
| **Display** | 128×160 IPS TFT, GC9107, RGB565, SPI @ 4 MHz |
| **Button** | PA7, active-LOW |
| **Battery** | 3.7 V LiPo, ~4.2 V full, measured via PB0 ADC |
| **Coil** | PB0 MOSFET gate — HIGH = fire |
| **Debug** | SWD (PA13/PA14) via ST-Link V2 |

Full pin table and peripheral map: [`docs/README.md`](docs/README.md)

---

## Repo Layout

```
Vaporware/
├── src/              Library — shared drivers used by every example
│   ├── include/      Public headers (display.h, button.h, battery.h, ...)
│   └── src/          Driver source files + startup.s
├── examples/         Ready-to-flash applications
│   ├── flappy/       Flappy Bird
│   ├── slots/        Slot machine with NV high-score
│   ├── diagnostic/   Hardware probe — checks every peripheral
│   ├── template/     Blank app skeleton — start here for a new project
│   └── streamer/     Live PC→display video streamer (screen capture, games)
├── tools/            Host-side scripts (flash, voltage check, diagnostics)
├── docs/             Hardware reference, reverse-engineering notes
└── firmware/         Raw device flash backups (not compiled source)
```

---

## Prerequisites

| Tool | Purpose |
|---|---|
| [Arm GNU Toolchain 14.2](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) | `arm-none-eabi-gcc` — cross-compiler |
| [OpenOCD](https://openocd.org) | Flash and debug via ST-Link (runs under WSL) |
| [ST-Link V2](https://www.amazon.com/s?k=st-link+v2) | SWD programmer (~$8) |
| WSL 2 + [usbipd-win](https://github.com/dorssel/usbipd-win) | Attach ST-Link USB to WSL for OpenOCD |

The toolchain is expected at:
```
C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\
```
If yours is elsewhere, update the `GCC` / `OBJCOPY` / `SIZE` lines at the top of the example's `build_*.bat`.

---

## Quick Start

### 1 — Wire the ST-Link

| ST-Link pin | Vape test pad |
|---|---|
| SWDIO | PA13 |
| SWCLK | PA14 |
| GND | GND |
| 3.3 V | — (vape is self-powered, leave disconnected) |

### 2 — Build

Open a Command Prompt, go to the example you want, and run the build script:

```cmd
cd examples\flappy
build_flappy.bat
```

Output lands in `examples\flappy\build\flappy.bin`.

### 3 — Flash

```cmd
python gen_direct_flash.py   :: generates direct_flash.tcl from the .bin
flash_vape.bat               :: attaches ST-Link to WSL, runs OpenOCD
```

The vape boots into the new firmware immediately after flashing.

---

## Examples

| Example | Description | Key features used |
|---|---|---|
| **flappy** | Flappy Bird clone | display, button, battery meter |
| **slots** | One-armed bandit with persistent high score | display, button, nv storage |
| **diagnostic** | Hardware probe — dumps all sensor readings to display | all modules |
| **template** | Skeleton app — copy this to start a new project | app framework |
| **streamer** | Stream live video from PC to the display via SWD (~7 fps) | display, SWD protocol, Python host |

The streamer example does not use the `app` framework — it implements its own `main()` and loops on an SWD-driven protocol. The companion host script (`stream_frames.py`) captures any window or screen region and pushes frames over ST-Link. See [`examples/streamer/README.md`](examples/streamer/README.md) for full setup and usage.

---

## Library Overview

The `src/` library provides everything needed to run apps. You never call `main()` — the framework does that. Just implement two functions:

```c
void app_init(void) {
    // called once at startup — set up your initial state and draw first frame
    display_fill(COL_BLACK);
}

void app_update(uint32_t frame) {
    // called ~30 times per second — update state, redraw changed regions
    if (button_just_pressed()) { /* ... */ }
}
```

| Module | What it does |
|---|---|
| `system` | 8 MHz HSI clock, TIM3 delay, TIM1 wall clock, IWDG feed |
| `display` | GC9107 init, fill, set window, draw pixel, draw image (RGB565) |
| `button` | PA7 debounce — pressed / just_pressed / just_released / held_ms |
| `battery` | PB0 ADC read, raw-to-voltage conversion, charge-level thresholds |
| `nv` | Write-forward NV storage in top 4 KB of flash — read / write / reset |
| `vape` | Coil safety init — holds PB0 LOW at reset to prevent accidental fire |
| `app` | `main()`, frame timer, sleep timeout, hold-to-reset, hardware init order |

Full API documentation: [`docs/README.md`](docs/README.md)

---

## Creating a New App

1. Copy `examples/template/` and rename the folder
2. Rename `build_template.bat` → `build_<yourapp>.bat` and set `APP_NAME`
3. Edit `src/main.c` — implement `app_init()` and `app_update()`
4. Build and flash using the same workflow as the examples above

The template build script already references `../../src` for the library — no path changes needed as long as your app lives under `examples/`.

---

## Flashing Tools

`tools/` contains host-side Python scripts for tasks beyond the normal build/flash flow:

| Script | Purpose |
|---|---|
| `flash_charge.py` | Flash any `.bin` via OpenOCD telnet (edit `BIN_PATH` at top) |
| `check_voltage.py` | Read live battery voltage via SWD without flashing |
| `diag_display.py` | Drive display init and fill sequences manually via OpenOCD |
| `cam_capture.py` | Capture a frame from a USB webcam (display debugging) |
| `spi_sniff.py` | Passive SPI transaction capture via SWD memory reads |

All scripts connect to an already-running OpenOCD telnet server on port 6666.

---

## Reverse Engineering Notes

The original Raz DC25000 firmware was reverse-engineered using Ghidra.
Findings (pin assignments, function addresses, display init sequence, flash memory map)
are documented in [`docs/FINDINGS.md`](docs/FINDINGS.md).
Raw firmware dumps and analysis scripts live in [`docs/reverse_engineering/`](docs/reverse_engineering/).
