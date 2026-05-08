# Streamer — Live Video Over SWD

Stream any PC window or screen region to the 128×160 vape display at ~7 fps
using the ST-Link SWD debugger as a video cable.

---

## How It Works

```
PC: stream_frames.py
    │  captures window/screen with PIL/mss
    │  scales to 64×80 logical pixels (BGR565)
    │  splits into 2 chunks of 64×40
    └─► OpenOCD write_memory (5128 B per chunk)
            │  via ST-Link SWD @ ~1 MHz
            ▼
        N32G031 SRAM @ 0x20000100
            │  MCU polls FAST_TRIG
            └─► display_draw_chunk_2x()
                    12 MHz SPI → GC9107 GRAM
                    2× pixel doubling → 128×80 physical pixels
```

The MCU boosts to 48 MHz PLL after display init, setting SPI to 12 MHz.
Each 64×40 logical chunk is blitted as 128×80 physical pixels (2× in both
axes), making one 5128-byte SWD write cover one quarter of the 128×160 screen.
Two chunks cover the full screen.

---

## Quick Start

### 1 — Flash the streamer firmware

```cmd
cd examples\streamer
build_streamer.bat
```

The display will show a green screen (PLL locked) then a cyan waiting screen.
Keep the ST-Link connected — it is used for both flashing and streaming.

### 2 — Run a test pattern

```cmd
python stream_frames.py --test --wsl
```

An animated rainbow gradient will stream to the display. Press Ctrl-C to stop.

### 3 — Stream a window

Launch the window you want to mirror, then:

```cmd
python stream_frames.py --window "MyApp" --wsl
```

The title match is case-insensitive and partial — `"doom"` matches
`"FreeDM - Chocolate Doom 3.1.1"`.

### 4 — Stream a screen region

```cmd
python stream_frames.py --screen 0 0 640 480 --wsl
```

Coordinates are `X Y W H` in pixels. Omit all four to capture the full
primary monitor.

### 5 — Halt and turn off the display

```cmd
python stream_frames.py --halt --wsl
```

---

## Playing FreeDM (Chocolate DOOM)

> **The DOOM binaries and WAD are not included in this repo** (gitignored).
> Download them once and place them in `examples/streamer/doom/chocolate-doom/`.

### Download

| File | Source |
|---|---|
| `chocolate-doom.exe` | [chocolate-doom.org/downloads](https://www.chocolate-doom.org/wiki/index.php/Downloads) — grab the Windows ZIP, extract `chocolate-doom.exe` |
| `freedm.wad` | [freedoom.github.io](https://freedoom.github.io/download.html) — grab the FreeDM ZIP, extract `freedm.wad` |

Place both files here:
```
examples/streamer/doom/chocolate-doom/
    chocolate-doom.exe
    freedm.wad
    chocolate-doom.cfg   ← already in repo (windowed, 533×400)
```

### Launch

```cmd
cd examples\streamer\doom\chocolate-doom
chocolate-doom.exe -iwad freedm.wad
```

Then in a second window:
```cmd
cd examples\streamer
python stream_frames.py --window "FreeDM" --wsl
```

Expected: ~6–7 fps. The game is fully playable — input goes to the DOOM window
as normal; the display just mirrors what DOOM renders.

---

## SWD Streaming Protocol

The MCU-side protocol is defined in `src/streamer.c` and the host-side in
`stream_frames.py`. All addresses are in MCU SRAM.

### SRAM Layout

| Address | Size | Name | Description |
|---|---|---|---|
| `0x20000010` | 4 B | `CTRL` | Control register (reset / sleep commands) |
| `0x20000100` | 4 B | `FAST_IDX` | Chunk index: 0 = top half, 1 = bottom half |
| `0x20000104` | 5120 B | `FAST_BUF` | 64×40 pixels, BGR565 little-endian |
| `0x20001504` | 4 B | `FAST_TRIG` | Write `0xCC` last to trigger MCU blit |

### Transfer Sequence (per chunk)

The host sends **one `write_memory` of 5128 bytes** to `0x20000100`.
Because `FAST_TRIG` is the last word in the payload, the MCU cannot act
before `FAST_BUF` is fully resident in SRAM:

```
write_memory 0x20000100 5128:
  [0000..0003]  FAST_IDX  = chunk index (0 or 1)
  [0004..1403]  FAST_BUF  = 5120 bytes of BGR565 pixel data (64×40 pixels)
  [1404..1407]  FAST_TRIG = 0x000000CC  ← written last
```

### CTRL Commands

| Value | Meaning |
|---|---|
| `0x00000000` | Idle (no command) |
| `0xDEAD0000` | Reset display (re-run `display_init()`) |
| `0xDEAD0001` | Sleep — turn off LCD backlight and halt MCU in IWDG loop |

### Chunk-to-Physical Mapping

| `FAST_IDX` | Physical rows on display |
|---|---|
| 0 | Top half (rows 0–79 physical = rows 0–39 logical) |
| 1 | Bottom half (rows 80–159 physical = rows 40–79 logical) |

The host sends chunk 1 (bottom) first, then chunk 0 (top). This minimises
the rolling-shutter window — the stale half during a frame transition is
always the top, which the MCU overwrites last.

---

## Rolling-Shutter Compensation

Because the two chunks are blitted ~25 ms apart, a naive frame can show
the top half from frame N-1 and the bottom from frame N, creating a
visible seam at the midpoint.

The test pattern (`--test`) applies a gradient coefficient
**K_Y = −10.8** to the vertical axis:

```
K_Y = -(LCD_H × T_frame × speed)
    = -(80 × 0.135 s × 1.0 cycle/s)
    = -10.8
```

With this value the temporal phase difference between the two halves
equals the spatial phase step, so the chunk boundary is identical to
every other row-to-row step — invisible. In the transition window (one
half old, one half new) the seam is mathematically zero.

For live window/screen streaming, rolling shutter is much less noticeable
because real content has coherent motion across the frame, and calibrating
K_Y to arbitrary content is impractical.

---

## Performance

| Metric | Value |
|---|---|
| Logical resolution | 64×80 pixels |
| Physical resolution | 128×160 pixels (2× upscaled) |
| SWD payload per frame | 10 256 B (2 × 5128 B) |
| SPI blit time per chunk | ~2.7 ms at 12 MHz |
| Frame rate | ~6–7 fps (limited by SWD/USB round-trip) |
| MCU clock | 48 MHz PLL (HSI×6) |
| SPI clock | 12 MHz (APB/4 at 48 MHz) |

---

## Python Dependencies

```cmd
pip install pillow mss numpy pywin32
```

| Package | Required for |
|---|---|
| `pillow` | All streaming modes (screen, window, video) |
| `mss` | Fast screen-region capture (`--screen`) |
| `numpy` | 10× faster BGR565 conversion (optional but recommended) |
| `pywin32` | Window capture by title (`--window`) |

OpenOCD must be installed in WSL (`sudo apt install openocd`) and accessible as
`openocd` on `$PATH`. The ST-Link is attached to WSL automatically via `usbipd`
on startup — the script hardcodes `--busid 1-2`. If your ST-Link is on a
different bus ID, find it with `usbipd list` (PowerShell) and edit the
`usbipd attach` line in `flash_vape.bat` and the equivalent call in
`stream_frames.py`.

---

## Architecture Notes

- The streamer does **not** use the `app` framework — it has its own `main()`
  and streaming loop.
- `bat_init()` is deliberately omitted: it writes `RCC_CFGR2` and disturbs the
  48 MHz PLL. The streamer never reads battery voltage.
- The IWDG is extended to ~17.5 s timeout during flash and fed every loop
  iteration during streaming, so a stuck PC does not cause a watchdog reset
  mid-game — it just freezes the last frame.
- If the display goes blank or corrupts, write `CTRL = 0xDEAD0000` via
  `stream_frames.py --halt` (which sends SLEEP), power-cycle the vape, and
  reflash.
