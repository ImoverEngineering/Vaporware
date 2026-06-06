# Streamer — Live Video Over SWD

Stream any PC screen, window, or video file to the 128×160 vape display at
up to ~8 fps using the ST-Link SWD debugger as a video cable.

---

## How It Works

```
PC: stream_frames.py
    │  mss grab thread: captures screen → downscales to 64×80 BGR565
    │  delta compression: only changed chunks are sent each frame
    │  WSL sidecar writes binary chunk to /tmp/vape_chunk.bin (tmpfs)
    └─► OpenOCD load_image (2568 B per chunk, no size limit)
            │  via ST-Link SWD @ 8 MHz
            ▼
        N32G031 SRAM @ 0x20000100
            │  MCU polls FAST_TRIG (written last — atomically safe)
            └─► display_draw_chunk_2x()
                    12 MHz SPI → GC9107 GRAM
                    2× pixel doubling → 128×40 physical rows per chunk
```

The screen is split into **4 chunks of 20 logical rows** (40 physical rows
each). Each chunk is sent as a single `load_image` binary transfer — no ASCII
hex encoding, no 230-word telnet limit. The MCU only acts when `FAST_TRIG` is
written, which is the last word in the payload, so the buffer is always fully
resident before the blit begins.

The MCU boosts to 48 MHz PLL after display init, setting SPI to 12 MHz. Each
64×20 logical chunk is blitted as 128×40 physical pixels (2× in both axes).

---

## Quick Start

### 1 — Flash the streamer firmware

```cmd
cd examples\streamer
build_streamer.bat
flash_vape.bat
```

The display shows green (PLL locked) then a cyan waiting screen.
Keep the ST-Link connected — it is used for both flashing and streaming.

### 2 — Run a test pattern

```cmd
python stream_frames.py --test --wsl
```

An animated rainbow gradient streams to the display. Press Ctrl-C to stop.

### 3 — Stream your screen

```cmd
python stream_frames.py --screen --wsl
```

Captures the full primary monitor. For a specific region (`X Y W H`):

```cmd
python stream_frames.py --screen 0 0 1280 720 --wsl
```

### 4 — Stream a window

```cmd
python stream_frames.py --window "MyApp" --wsl
```

Title matching is case-insensitive and partial — `"doom"` matches
`"FreeDM - Chocolate Doom 3.1.1"`.

### 5 — Stream a video file

```cmd
python stream_frames.py --video myvideo.mp4 --wsl
```

Requires `ffmpeg` on `$PATH`.

### 6 — Halt and turn off the display

```cmd
python stream_frames.py --halt --wsl
```

---

## Image Options

### Crop

Crop a fraction of the source image before downscaling. Values are 0.0–1.0
fractions of the source width/height (`LEFT TOP RIGHT BOTTOM`):

```cmd
# Centre third of the screen (good for portrait video in the middle monitor)
python stream_frames.py --screen --wsl --crop 0.333 0 0.667 1
```

### Rotate

Rotate the image before display. Useful for holding the vape sideways:

```cmd
python stream_frames.py --screen --wsl --rotate 90
python stream_frames.py --screen --wsl --rotate 270
```

The rotation is applied before downscaling so it costs nothing extra at
display resolution.

### Posterize

Reduce effective colour depth by zeroing the N low bits of each RGB channel
before BGR565 packing. This collapses nearby colours (video compression
artefacts, noise, subtle gradients) into the same value, so more chunks
compare equal between frames — the delta compressor skips more, raising
effective fps on video content.

```cmd
python stream_frames.py --screen --wsl --posterize 2   # recommended for video
python stream_frames.py --screen --wsl --posterize 4   # heavy, retro look
```

| `--posterize` | Colours per channel | Effect |
|---|---|---|
| 0 (default) | 32 / 64 / 32 (full BGR565) | No change |
| 2 | 64 → 16 per channel | Subtle, improves delta hits on compressed video |
| 4 | 16 → 4 per channel | Visible posterization, maximum delta compression |

### Temporal Smoothing

Blend a fraction of the previous frame into the current frame. At ~7 fps,
discrete frame jumps can look choppy; blending produces ghost-trail motion
that reads as smoother. It also reduces per-pixel variation, further helping
the delta compressor.

```cmd
python stream_frames.py --screen --wsl --smooth 0.3    # light smoothing
python stream_frames.py --screen --wsl --smooth 0.5    # heavier ghost trail
```

`--smooth` and `--posterize` combine well:

```cmd
python stream_frames.py --screen --wsl --posterize 2 --smooth 0.3
```

---

## Playing FreeDM (Chocolate DOOM)

> **The DOOM binaries and WAD are not included in this repo** (gitignored).

### Download

| File | Source |
|---|---|
| `chocolate-doom.exe` | [chocolate-doom.org/downloads](https://www.chocolate-doom.org/wiki/index.php/Downloads) |
| `freedm.wad` | [freedoom.github.io](https://freedoom.github.io/download.html) |

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

Then in a second terminal:
```cmd
cd examples\streamer
python stream_frames.py --window "FreeDM" --wsl
```

Expected: ~6–8 fps. Input goes to the DOOM window as normal; the display
mirrors what DOOM renders.

---

## SWD Streaming Protocol

The MCU-side protocol lives in `src/streamer.c`; the host side in
`stream_frames.py`. All addresses are in MCU SRAM.

### SRAM Layout (4-chunk mode)

| Address | Size | Name | Description |
|---|---|---|---|
| `0x20000010` | 4 B | `CTRL` | Control register (reset / sleep commands) |
| `0x20000100` | 4 B | `FAST_IDX` | Chunk index 0–3 (0 = top, 3 = bottom) |
| `0x20000104` | 2560 B | `FAST_BUF` | 64×20 pixels, BGR565 little-endian |
| `0x20000B04` | 4 B | `FAST_TRIG` | Write `0xCC` last to trigger MCU blit |

### Transfer Sequence (per chunk)

The host sends **one `load_image` of 2568 bytes** to `0x20000100` via a WSL
sidecar that writes the binary to `/tmp/vape_chunk.bin` (tmpfs) for OpenOCD
to read. Because `FAST_TRIG` is the last word, the MCU cannot act before
`FAST_BUF` is fully resident in SRAM:

```
load_image /tmp/vape_chunk.bin 0x20000100 bin  (2568 bytes):
  [0000..0003]  FAST_IDX  = chunk index (0–3)
  [0004..0A03]  FAST_BUF  = 2560 bytes of BGR565 pixel data (64×20 pixels)
  [0A04..0A07]  FAST_TRIG = 0x000000CC  ← written last
```

### CTRL Commands

| Value | Meaning |
|---|---|
| `0x00000000` | Idle |
| `0xDEAD0000` | Reset display (re-run `display_init()`) |
| `0xDEAD0001` | Sleep — turn off LCD and halt MCU |

### Chunk-to-Physical Mapping

| `FAST_IDX` | Logical rows | Physical rows |
|---|---|---|
| 0 | 0–19 | 0–39 |
| 1 | 20–39 | 40–79 |
| 2 | 40–59 | 80–119 |
| 3 | 60–79 | 120–159 |

Chunks are sent in reverse order (3 → 0) to match the GC9107 scan direction
(`MADCTL=0x98`, `ML=1`): the gate scan runs from row 159 down to 0, so writing
in the same direction minimises the tearing window.

---

## Delta Compression

`send_frame()` compares each chunk's byte content to the previous frame. If
a chunk is unchanged, the `load_image` for that chunk is skipped entirely.
On mostly-static content (e.g. a game menu, paused video) this can push
effective frame rate above 30 fps for the chunks that do change.

Every 30 frames the cache is invalidated and all 4 chunks are force-sent,
so any artefact from an SWD glitch or MCU reset clears within ~1 second.

---

## Performance

| Metric | Value |
|---|---|
| Logical resolution | 64×80 pixels |
| Physical resolution | 128×160 pixels (2× upscaled in both axes) |
| Chunks per frame | 4 (each 64×20 logical / 128×40 physical rows) |
| SWD payload per chunk | 2568 B (IDX + BUF + TRIG) |
| SPI blit time per chunk | ~6.8 ms at 12 MHz |
| Frame rate — fully-changing | ~7–8 fps |
| Frame rate — mostly-static | up to ~30 fps (delta compression) |
| MCU clock | 48 MHz PLL (HSI×6) |
| SPI clock | 12 MHz (APB/4 at 48 MHz) |
| Screen capture overhead | ~0 ms (hidden by background grab thread) |

### Why 64×80 instead of 128×160?

The N32G031 has only 8 KB SRAM. A single full-resolution row is 128×2 = 256
bytes; a 20-row chunk at full resolution would be 5120 bytes, leaving barely
3 KB for stack and variables. With 8 or more chunks required at full res, each
`load_image` round-trip takes 2–3× longer and the frame rate drops to ~3 fps.

The 2× pixel mode (64×80 logical) cuts the SWD payload in half while producing
the same 128×160 physical pixels on SPI — blit time is identical either way.
The fps gain is entirely on the SWD transfer side.

---

## Architecture Notes

- **Threaded screen capture**: `screen_frames()` runs mss grab and BGR565
  conversion in a background thread. Capture (~30 ms on a 4K screen) overlaps
  with the SWD send, contributing zero overhead to the critical path.

- **load_image vs write_memory**: The OpenOCD telnet `write_memory` command
  silently drops payloads above ~230 words (~920 bytes). `load_image` reads a
  binary file from the WSL filesystem — no size limit, no ASCII hex encoding,
  single round-trip per chunk.

- **Windows timer resolution**: `timeBeginPeriod(1)` is called on startup to
  reduce the scheduler quantum from 15.6 ms to ~1 ms. Without it, each
  `load_image` response arrives on a 15.6 ms boundary, quantising chunk
  timings to multiples of ~16 ms regardless of actual SWD speed.

- **ST-Link auto-recovery**: If OpenOCD fails to connect on startup (typically
  because a previous session died mid-transaction, leaving the ST-Link adapter
  in a stale state), `_connect()` automatically cycles the USB attachment via
  `usbipd detach` → `usbipd attach` to reset the adapter, then retries once.

- **No `bat_init()`**: The streamer deliberately omits battery init — it writes
  `RCC_CFGR2` which disturbs the 48 MHz PLL. The streamer never reads battery
  voltage so it is safe to skip.

- **IWDG**: Fed every streaming loop iteration. A stuck or crashed PC freezes
  the last frame on the display rather than triggering a watchdog reset.

---

## Python Dependencies

```cmd
pip install pillow mss numpy pywin32
```

| Package | Required for |
|---|---|
| `pillow` | All streaming modes |
| `mss` | Fast screen capture (`--screen`) |
| `numpy` | ~10× faster BGR565 conversion; required for `--smooth` |
| `pywin32` | Window capture by title (`--window`) |

OpenOCD must be installed in WSL (`sudo apt install openocd`).
The ST-Link is attached to WSL automatically via `usbipd` — the script
hardcodes bus ID `1-2`. Find yours with `usbipd list` (PowerShell) and
update `USBIPD_BUSID` in `stream_frames.py` and the attach line in
`flash_vape.bat` if it differs.
