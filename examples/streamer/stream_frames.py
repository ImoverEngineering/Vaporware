#!/usr/bin/env python3
"""stream_frames.py — Stream frames to the N32G031 vape display via SWD.

Requires: pip install pillow mss
OpenOCD must be installed in WSL (used instead of pyocd — more reliable on
this hardware). The ST-Link is attached to WSL automatically on startup.

Modes:
    --test              Animated rainbow test pattern (no other software needed)
    --screen [X Y W H] Capture a screen region and stream it live
                        Default region: full primary monitor
    --window TITLE      Capture a specific window by title (partial match OK)
    --video FILE        Stream frames from a video file (requires ffmpeg in PATH)
    --file PATH         Read raw RGB24 frames from a file written by screen_capture.py
    --halt              Turn off the LCD and halt the MCU (no streaming)

Usage examples:
    python stream_frames.py --screen
    python stream_frames.py --window "DOOM"
    python stream_frames.py --video clip.mp4
    python stream_frames.py --halt

Protocol (see streamer.c for MCU side):
    Fast streaming layout — one write_memory per chunk:

    FAST_IDX  @ 0x20000100   chunk index 0–9
    FAST_BUF  @ 0x20000104   1024 bytes of logical pixel data (64×8 px, BGR565 LE)
    FAST_TRIG @ 0x20000504   0xCC written LAST — MCU blits chunk, clears to 0

    OpenOCD load_image /tmp/vape_chunk.bin 0x20000100 bin
    FAST_TRIG is the final word — MCU cannot fire before BUF is fully written.

    Legacy CTRL @ 0x20000010:
        0xDEAD0000 = reset display
        0xDEAD0001 = sleep display + idle loop (for halt)
"""

import argparse
import os
import random
import re
import socket
import struct
import subprocess
import sys
import time

# ── Dependencies check ────────────────────────────────────────────────────────
try:
    from PIL import Image, ImageGrab
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import mss
    HAS_MSS = True
except ImportError:
    HAS_MSS = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

# ── Protocol ──────────────────────────────────────────────────────────────────
# Half-resolution 2× mode: 64×80 logical pixels → 128×160 physical pixels.
# Each logical pixel is blitted as a 2×2 block by display_draw_chunk_2x().
# SWD payload per chunk: 4 (IDX) + 1024 (BUF 64×8×2) + 4 (TRIG) = 1032 bytes.
# At ~150 KB/s effective hla_swd throughput: ~7ms SWD + ~11ms SPI = ~18ms/chunk
# → ~6fps for full-frame updates vs ~3fps in full-resolution mode.
FAST_IDX_ADDR  = 0x20000100
FAST_BUF_ADDR  = 0x20000104   # IDX(4) + 5120 bytes logical pixel data (64×40×2)
FAST_TRIG_ADDR = 0x20001504   # IDX(4) + BUF(5120) bytes past FAST_IDX_ADDR
CTRL_ADDR      = 0x20000010   # legacy: reset / sleep

_STREAMER_DIR  = os.path.dirname(os.path.abspath(__file__))

CTRL_IDLE  = 0x00000000
CTRL_CHUNK = 0x000000CC
CTRL_RESET = 0xDEAD0000
CTRL_SLEEP = 0xDEAD0001

# 2-chunk mode: 40 logical rows per chunk (80 physical rows after 2× scale).
# 2 USB round-trips per frame instead of 5 → ~2.5× fps improvement (~30fps).
CHUNK_ROWS = 40
NUM_CHUNKS = 2
LCD_W, LCD_H = 64, 80   # logical resolution (displayed as 128×160 via 2× scale)

# ── OpenOCD / WSL config ──────────────────────────────────────────────────────
OCD_TARGET_CFG = (
    "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/streamer/"
    "n32g031.openocd.cfg"
)
OCD_WIN_EXE = r"C:\Program Files\OpenOCD\bin\openocd.exe"
# Windows path to the same cfg file (used by native Windows OpenOCD)
OCD_WIN_CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "n32g031.openocd.cfg")
USBIPD_BUSID = "1-2"
OCD_TELNET_PORT = 4444


# ── Color conversion ──────────────────────────────────────────────────────────
def image_to_chunks(img):
    """Convert any PIL Image → 10 raw chunk byte-strings (4096 B each, BGR565 LE).

    Uses numpy for fast vectorised BGR565 packing when available (~10× faster
    than the pure-Python fallback).  The fallback is kept for environments
    where numpy is not installed.
    """
    img = img.convert("RGB")
    if img.size != (LCD_W, LCD_H):
        img = img.resize((LCD_W, LCD_H), Image.LANCZOS)

    if HAS_NUMPY:
        # Shape: (LCD_H, LCD_W, 3) — dtype uint8
        arr = np.frombuffer(img.tobytes(), dtype=np.uint8).reshape(LCD_H, LCD_W, 3)
        r = arr[:, :, 0].astype(np.uint16)
        g = arr[:, :, 1].astype(np.uint16)
        b = arr[:, :, 2].astype(np.uint16)
        # BGR565: b[4:0] g[5:0] r[4:0]  packed as little-endian uint16
        bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
        # Force little-endian byte order then view as raw bytes
        bgr565_le = bgr565.astype('<u2')
        raw = bgr565_le.tobytes()   # (LCD_H * LCD_W * 2) bytes, row-major
        return [raw[ci * LCD_W * CHUNK_ROWS * 2 : (ci + 1) * LCD_W * CHUNK_ROWS * 2]
                for ci in range(NUM_CHUNKS)]

    # Pure-Python fallback (no numpy)
    data = img.tobytes()
    chunks = []
    for ci in range(NUM_CHUNKS):
        row0 = ci * CHUNK_ROWS
        buf = bytearray(LCD_W * CHUNK_ROWS * 2)
        off = 0
        for row in range(CHUNK_ROWS):
            src = ((row0 + row) * LCD_W) * 3
            for col in range(LCD_W):
                r = data[src];     src += 1
                g = data[src];     src += 1
                b = data[src];     src += 1
                val = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
                struct.pack_into('<H', buf, off, val)
                off += 2
        chunks.append(bytes(buf))
    return chunks


# ── OpenOCD telnet backend ────────────────────────────────────────────────────
_ANSI_RE = re.compile(rb'\x1b\[[^m]*m')

class VapeDisplay:
    """Controls the vape display via OpenOCD running in WSL.

    Startup sequence:
      1. usbipd attach --wsl to give WSL access to the ST-Link.
      2. Launch OpenOCD in WSL background process.
      3. Connect to OpenOCD telnet server on localhost:4444.
      4. Resume target — firmware runs through display_init() (~700 ms).

    Each send_frame() call writes one write_memory block per changed chunk
    (IDX + BUF + TRIG in a single SWD bulk transfer).
    """

    # Path where the chunk writer sidecar stores the current chunk in WSL's
    # native /tmp filesystem (tmpfs — instant read by OpenOCD load_image).
    _WSL_CHUNK_PATH = '/tmp/vape_chunk.bin'

    def __init__(self, freq=4000000):
        self._prev_chunks = [None] * NUM_CHUNKS
        self._freq_khz = freq // 1000
        self._sock = None
        self._ocd_proc = None
        self._wsl_keepalive = None  # persistent WSL process; keeps WSL VM alive so usbipd doesn't detach
        self._wsl_writer = None    # sidecar: receives chunk bytes, writes to WSL /tmp
        self._rxbuf = b''   # persistent receive buffer for telnet stream
        self._connect()

    def _connect(self):
        # Start WSL first — usbipd attach requires a running WSL 2 instance
        print("  Starting WSL…")
        subprocess.run(['wsl', 'echo', 'ready'], capture_output=True, timeout=10)

        # Kill any stale OpenOCD from a previous session.  When Python is killed
        # externally (taskkill / Ctrl-C), the WSL child processes keep running and
        # hold port 4444, causing the next session to fail with rc=1 immediately.
        subprocess.run(
            ['wsl', '-u', 'root', '--', 'pkill', '-9', '-f', 'openocd'],
            capture_output=True
        )
        time.sleep(0.3)  # brief wait for port to release

        # Keep WSL alive for the entire session.  WSL 2 shuts down ~8 s after
        # the last process exits; when it shuts down usbipd detaches the
        # ST-Link and subsequent OpenOCD launches fail with "Error: open failed".
        # A background `sleep infinity` keeps the VM running until close().
        self._wsl_keepalive = subprocess.Popen(
            ['wsl', '-u', 'root', 'bash', '-c', 'sleep infinity'],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.5)  # brief wait for the keepalive process to settle in WSL

        # Launch WSL chunk-writer sidecar.  Python (Windows) can't write directly
        # to WSL's native /tmp filesystem; going through /mnt/c/ adds ~50 ms of
        # cross-filesystem overhead per chunk.  This sidecar reads length-prefixed
        # chunk bytes from its stdin, writes them to /tmp/vape_chunk.bin (tmpfs,
        # instant), then prints "ok\n" so the caller knows the file is ready.
        _sidecar_py = (
            'import sys,struct\n'
            'p="/tmp/vape_chunk.bin"\n'
            'while True:\n'
            '  hdr=sys.stdin.buffer.read(4)\n'
            '  if len(hdr)<4: break\n'
            '  n=struct.unpack("<I",hdr)[0]\n'
            '  buf=bytearray()\n'
            '  while len(buf)<n:\n'
            '    c=sys.stdin.buffer.read(n-len(buf))\n'
            '    if not c: break\n'
            '    buf+=c\n'
            '  if not buf: break\n'
            '  open(p,"wb").write(bytes(buf))\n'
            '  sys.stdout.write("ok\\n"); sys.stdout.flush()\n'
        )
        self._wsl_writer = subprocess.Popen(
            ['wsl', '-u', 'root', 'python3', '-c', _sidecar_py],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

        # Now attach ST-Link to WSL
        print("  Attaching ST-Link to WSL…")
        result = subprocess.run(
            ['usbipd', 'attach', '--wsl', '--busid', USBIPD_BUSID],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"  WARNING: usbipd attach returned {result.returncode}: {result.stderr.strip()}")
        time.sleep(1.5)

        # Launch OpenOCD in WSL
        print("  Starting OpenOCD in WSL…")
        # Override stm32f0x_default_reset_init before loading n32g0x.cfg.
        # That hook boosts SYSCLK to 48 MHz while the MCU is halted.  When
        # firmware then runs clock_init() it assumes 8 MHz and sets TIM3
        # PSC=7, making all delays 6× too short.  The GC9107 Sleep Out wait
        # (nominally 120 ms) becomes ~20 ms and the display never wakes.
        # Overriding the proc to a no-op keeps the MCU at 8 MHz HSI so
        # the firmware's own clock_boost_48mhz() runs correctly after display_init.
        ocd_cmd = (
            f'openocd -f {OCD_TARGET_CFG} '
            f'-c "init" '
            f'-c "reset halt"'
        )
        # stdin=PIPE prevents OpenOCD from reading the parent's stdin.
        # Without this, if the parent's stdin is a closed pipe (common in
        # scripted/automated contexts), OpenOCD reads EOF and shuts itself
        # down within seconds of starting.  Keeping an open write-end alive
        # makes OpenOCD stay in server mode indefinitely.
        self._ocd_proc = subprocess.Popen(
            ['wsl', '-u', 'root', 'bash', '-c', ocd_cmd],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Wait for OpenOCD to connect to target and open telnet port.
        # n32g0x.cfg runs init + reset halt before the server loop starts;
        # 5s is safe even on a cold WSL start.
        time.sleep(5.0)

        # Connect to OpenOCD telnet
        print("  Connecting to OpenOCD telnet…")
        rc = self._ocd_proc.poll()
        if rc is not None:
            try:
                log = subprocess.run(
                    ['wsl', 'cat', '/tmp/openocd_stream.log'],
                    capture_output=True, text=True, timeout=3
                ).stdout
            except Exception:
                log = "(log unavailable)"
            raise RuntimeError(f"OpenOCD exited early (rc={rc}):\n{log}")
        self._sock = socket.create_connection(('localhost', OCD_TELNET_PORT), timeout=5)
        self._sock.settimeout(15)
        self._read_prompt()   # consume welcome banner + first prompt

        # Custom cfg sets adapter speed 2000; boost to streaming speed now.
        self._cmd(f'adapter speed {self._freq_khz}')

        # Resume firmware — custom cfg left MCU halted after reset halt.
        # Firmware runs: clock_init → display_init (~400 ms) →
        # clock_boost_48mhz (PLL) → GREEN fill + delay_ms(2000) →
        # draw_waiting (3 × ~220 ms fills) → streaming loop.
        # OpenOCD briefly loses SWD when PLL switches; auto-reconnects in <100 ms.
        # 5 s covers the full 2 s delay_ms + draw_waiting fills with margin.
        print("  Resuming firmware (waiting for boot ~5 s)…")
        self._cmd('resume')
        time.sleep(5.0)

        # Drain any stale data OpenOCD printed during the boot wait.
        # The PLL glitch causes a brief SWD comms failure; OpenOCD logs
        # error/retry messages containing '>' to the telnet socket.  Those
        # accumulate and cause _read_prompt() to return prematurely for the
        # first chunk sends, so FAST_TRIG never gets set and blits are skipped.
        self._sock.settimeout(0.1)
        try:
            while self._sock.recv(4096):
                pass
        except socket.timeout:
            pass
        self._rxbuf = b''
        self._sock.settimeout(15)
        self._cmd('echo synced')   # re-sync: guarantees next _read_prompt waits for a fresh prompt

    def _read_prompt(self, timeout=5.0):
        """Consume exactly one OpenOCD '>' prompt from the receive stream.

        Uses a persistent _rxbuf so pipelined commands work correctly:
        if multiple responses arrived before this call, we consume only
        the first prompt and leave the rest buffered for the next call.
        """
        deadline = time.monotonic() + timeout
        self._sock.settimeout(0.05)
        try:
            while True:
                # Strip ANSI from accumulated buffer and look for first '>'
                stripped = _ANSI_RE.sub(b'', self._rxbuf)
                idx = stripped.find(b'>')
                if idx >= 0:
                    # Found one prompt — return it, keep remainder buffered
                    response = stripped[:idx + 1].decode('utf-8', errors='replace')
                    self._rxbuf = stripped[idx + 1:]   # keep rest (already stripped)
                    return response
                if time.monotonic() >= deadline:
                    break
                # Need more data
                try:
                    data = self._sock.recv(4096)
                    if not data:
                        break
                    self._rxbuf += data
                except socket.timeout:
                    pass
        finally:
            self._sock.settimeout(15)
        # Timeout fallback
        result = _ANSI_RE.sub(b'', self._rxbuf).decode('utf-8', errors='replace')
        self._rxbuf = b''
        return result

    def _cmd(self, command):
        """Send one OpenOCD telnet command and wait for the prompt."""
        self._sock.sendall((command + '\n').encode())
        return self._read_prompt()

    def _sidecar_write(self, data):
        """Write variable-length binary data to the WSL sidecar (length-prefixed)."""
        self._wsl_writer.stdin.write(struct.pack('<I', len(data)))
        self._wsl_writer.stdin.write(data)
        self._wsl_writer.stdin.flush()
        self._wsl_writer.stdout.readline()   # block until sidecar acks

    def _send_chunk(self, idx, chunk):
        """Write one chunk to the MCU via a single load_image, then wait for blit.

        Sends [IDX(4)][BUF(N)][TRIG(4)] as a binary file to WSL /tmp via the
        sidecar, then issues load_image.  TRIG is the last word written, so the
        MCU cannot fire before BUF is fully in SRAM.

        After load_image completes (SWD write done), the MCU begins blitting
        FAST_BUF to the display over SPI.  We MUST wait for this blit to finish
        before writing the next chunk, otherwise the next SWD write overwrites
        FAST_BUF while the MCU is still reading it (race condition → corrupted
        display and unpredictable half-screen freezes).

        The wait uses a TCL while-loop that runs INSIDE OpenOCD (zero extra USB
        round-trips per poll).  The MCU clears FAST_TRIG to 0 after the blit;
        the loop returns as soon as that happens.  Cost: ~14ms (blit time) +
        one telnet round-trip (~3ms).
        """
        packet = struct.pack('<I', idx) + chunk + struct.pack('<I', CTRL_CHUNK)
        self._sidecar_write(packet)
        self._sock.sendall(
            f'load_image {self._WSL_CHUNK_PATH} 0x{FAST_IDX_ADDR:08X} bin\n'.encode()
        )
        self._read_prompt()
        # Wait for MCU blit to finish before next SWD write touches FAST_BUF.
        # Blit time = 40 rows × 80 phys rows × 128px × 2B × 8 / 12 MHz ≈ 14ms.
        # Fixed sleep is simpler and cheaper than polling (each mrw costs ~10ms
        # of SWD overhead, making a poll loop far slower than a plain sleep).
        time.sleep(0.025)   # 14ms blit + 11ms margin (Windows timer ~15.6ms grain)

    def send_frame(self, chunks):
        """Send all changed chunks. Returns (elapsed_seconds, chunks_sent).

        5-chunk mode: ~13ms/chunk → ~15fps for full-frame updates (vs ~8fps
        with 10 chunks).  Delta skipping means static content runs faster.
        """
        t0 = time.monotonic()
        chunks_sent = 0

        # Write chunks in REVERSED order (4→0) to match the GC9107 scan direction.
        # MADCTL=0x98 → ML=1 → gate scan runs GRAM row 159→0, i.e. chunk 4→0.
        # Writing in the same direction as the scan means only chunk 4 (top 16
        # logical rows = physical top of screen) can tear; chunks 0-3 are written
        # after the scan has already passed through them and don't tear.
        for idx in range(NUM_CHUNKS - 1, -1, -1):
            chunk = chunks[idx]
            if chunk == self._prev_chunks[idx]:
                continue
            self._prev_chunks[idx] = chunk
            self._send_chunk(idx, chunk)
            chunks_sent += 1

        return time.monotonic() - t0, chunks_sent

    def send_frame_live(self, chunk_gen):
        """Like send_frame() but generates chunk data immediately before each send.

        Captures one shared frame timestamp at the start, then passes it to
        every chunk_gen(idx, frame_t) call.  Because all chunks use the same
        virtual time the colour at every chunk boundary is continuous (only the
        natural y-gradient, ~1 palette step), making the seam invisible.

        The two chunks are still written to GRAM ~62ms apart (unavoidable SWD
        latency), but since the content was rendered for the same animation
        instant the boundary is seamless and the display looks like a coherent
        snapshot rather than a stitched multi-timestamp frame.

        Used by --test mode with numpy; all other modes use send_frame().
        """
        frame_t = time.monotonic()   # shared timestamp — same for every chunk
        t0 = frame_t
        chunks_sent = 0
        for idx in range(NUM_CHUNKS - 1, -1, -1):
            chunk = chunk_gen(idx, frame_t)
            if chunk == self._prev_chunks[idx]:
                continue
            self._prev_chunks[idx] = chunk
            self._send_chunk(idx, chunk)
            chunks_sent += 1
        return time.monotonic() - t0, chunks_sent

    def sleep_and_halt(self):
        """Write CTRL_SLEEP, wait for display_sleep_in(), then halt the CPU."""
        self._cmd(f'mww 0x{CTRL_ADDR:08X} 0x{CTRL_SLEEP:08X}')
        time.sleep(0.25)
        self._cmd('halt')
        print("  MCU halted, LCD off.")

    def reset_display(self):
        """Force MCU to re-init the GC9107 display."""
        self._cmd(f'mww 0x{CTRL_ADDR:08X} 0x{CTRL_RESET:08X}')
        self._prev_chunks = [None] * NUM_CHUNKS

    def close(self):
        try:
            self._cmd('shutdown')
        except Exception:
            pass
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if self._ocd_proc:
            try:
                self._ocd_proc.wait(timeout=3)
            except Exception:
                self._ocd_proc.terminate()
        if self._wsl_writer:
            try:
                self._wsl_writer.stdin.close()
                self._wsl_writer.wait(timeout=2)
            except Exception:
                self._wsl_writer.terminate()
        if self._wsl_keepalive:
            try:
                self._wsl_keepalive.terminate()
            except Exception:
                pass


# ── Native Windows OpenOCD backend (no WSL, no sidecar, no file I/O) ─────────
class VapeDisplayNative(VapeDisplay):
    """Like VapeDisplay but runs OpenOCD natively on Windows.

    Differences vs VapeDisplay:
      • No WSL, no usbipd, no sidecar subprocess.
      • Detaches ST-Link from WSL (if attached) so Windows can claim it.
      • Writes chunk data as inline hex via OpenOCD write_memory — no temp file.
      • Lower per-chunk overhead: ~3-4ms vs ~10ms → ~3× more frames/sec.

    write_memory format:
        write_memory ADDR 32 { 0xWORD0 0xWORD1 ... }
      The 1032-byte chunk packet (IDX + BUF + TRIG) is 258 32-bit LE words.
      TRIG is the last word, so MCU cannot fire before BUF is fully written,
      same race-condition guarantee as load_image.
    """

    def _connect(self):
        # Detach ST-Link from WSL so Windows can claim it
        print("  Detaching ST-Link from WSL (if attached)…")
        subprocess.run(
            ['usbipd', 'detach', '--busid', USBIPD_BUSID],
            capture_output=True,
        )
        time.sleep(0.5)

        # Kill any stale Windows OpenOCD
        subprocess.run(['taskkill', '/F', '/IM', 'openocd.exe'],
                       capture_output=True)
        time.sleep(0.3)

        # Launch Windows-native OpenOCD
        print("  Starting Windows OpenOCD…")
        self._ocd_proc = subprocess.Popen(
            [OCD_WIN_EXE,
             '-f', OCD_WIN_CFG,
             '-c', 'init',
             '-c', 'reset halt'],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # No WSL/usbipd needed — OpenOCD talks to ST-Link directly over WinUSB.
        # 4s covers init + reset halt on native Windows (no virtualisation).
        time.sleep(4.0)

        print("  Connecting to OpenOCD telnet…")
        rc = self._ocd_proc.poll()
        if rc is not None:
            raise RuntimeError(f"Windows OpenOCD exited early (rc={rc}). "
                               "Check ST-Link connection and drivers.")
        self._sock = socket.create_connection(('localhost', OCD_TELNET_PORT), timeout=5)
        self._sock.settimeout(15)
        self._read_prompt()

        self._cmd(f'adapter speed {self._freq_khz}')

        print("  Resuming firmware (waiting for boot ~5 s)…")
        self._cmd('resume')
        time.sleep(5.0)

    def _send_chunk(self, idx, chunk):
        """Write chunk via write_memory with inline hex — no file, no sidecar."""
        packet = struct.pack('<I', idx) + chunk + struct.pack('<I', CTRL_CHUNK)
        # 1032 bytes = 258 32-bit LE words
        words = struct.unpack(f'<{len(packet) // 4}I', packet)
        hex_words = ' '.join(f'0x{w:08X}' for w in words)
        self._cmd(f'write_memory 0x{FAST_IDX_ADDR:08X} 32 {{ {hex_words} }}')

    def close(self):
        try:
            self._cmd('shutdown')
        except Exception:
            pass
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if self._ocd_proc:
            try:
                self._ocd_proc.wait(timeout=3)
            except Exception:
                self._ocd_proc.terminate()


# ── Frame providers ───────────────────────────────────────────────────────────
def test_frames():
    """Animated rainbow — one full hue cycle per second using wall-clock time."""
    import colorsys
    # Pre-build the palette once (256 entries) and animate by rotating the offset.
    # This is ~50× faster than calling hsv_to_rgb per pixel per frame.
    palette = [tuple(int(c * 255) for c in colorsys.hsv_to_rgb(i / 256.0, 1.0, 1.0))
               for i in range(256)]

    if HAS_NUMPY:
        # Numpy fast path: ~0.5ms/frame vs ~10ms for the Python loop below.
        # Pre-compute the static spatial component once; just add t per frame.
        pal_np = np.array(palette, dtype=np.uint8)   # shape (256, 3)
        y_frac = np.arange(LCD_H, dtype=np.float32)[:, np.newaxis] / LCD_H * 0.3
        x_frac = np.arange(LCD_W, dtype=np.float32)[np.newaxis, :] / LCD_W
        base_h = x_frac + y_frac  # (LCD_H, LCD_W) — static diagonal pattern

        # ── Temporal compensation ────────────────────────────────────────────
        # Chunks are written sequentially (9→0) over ~110ms per frame.
        # Each chunk therefore reaches GRAM at a different wall-clock time.
        # Without compensation every displayed frame is a mix of N different
        # animation timestamps — one per chunk — creating visible horizontal
        # colour bands (the "stagnant then big jump" artifact).
        #
        # Fix: pre-advance each row's render time by the amount its chunk will
        # lag behind the first-written chunk (chunk 9, send position 0).
        #   chunk ci → send position (NUM_CHUNKS-1-ci) → lag (9-ci)×CHUNK_DELAY_S
        # This way, when the scan reads chunk ci at time T + lag, the content
        # was rendered for exactly time T+lag → temporally consistent display.
        #
        # Residual tear drops from 1 frame-period (125ms, very visible) to
        # ~17ms (1.7% of a 1Hz cycle, effectively invisible), and that
        # single-row glitch drifts randomly because of the phase-dither sleep.
        CHUNK_DELAY_S = 0.012   # ≈12ms measured per chunk at 8MHz SWD
        chunk_idx = np.arange(LCD_H, dtype=np.float32) // CHUNK_ROWS  # 0–9
        send_pos  = (NUM_CHUNKS - 1) - chunk_idx   # 9 for y=0 (chunk 0), 0 for y=79 (chunk 9)
        # Shape (LCD_H, 1) — broadcasts across columns
        t_offsets = (send_pos * CHUNK_DELAY_S).astype(np.float32)[:, np.newaxis]

        t0 = time.monotonic()
        while True:
            t = time.monotonic() - t0
            # Each row gets its own time offset so the rendered content matches
            # the animation time when that row's chunk will actually hit GRAM.
            h_idx = ((base_h + t + t_offsets) % 1.0 * 256).astype(np.int32) % 256
            rgb = pal_np[h_idx]   # (LCD_H, LCD_W, 3) uint8
            yield Image.fromarray(rgb, 'RGB')
        return

    # Pure-Python fallback (no numpy) — ~10ms/frame
    t0 = time.monotonic()
    while True:
        t = time.monotonic() - t0  # seconds since start
        img = Image.new("RGB", (LCD_W, LCD_H))
        px = img.load()
        for y in range(LCD_H):
            for x in range(LCD_W):
                # Diagonal rainbow that scrolls at 1 cycle/second
                h_idx = int(((x / LCD_W + y / LCD_H * 0.3 + t) % 1.0) * 256) % 256
                px[x, y] = palette[h_idx]
        yield img


def test_chunk_gen():
    """Live per-chunk rainbow generator — renders each chunk at its actual send time.

    Returns a callable gen(ci) that, when called, samples time.monotonic() and
    renders only the 8 rows belonging to chunk ci at that exact moment.

    Why this beats the fixed-offset approach:
      Fixed offsets (CHUNK_DELAY_S) must be estimated in advance and never match
      the actual USB/SWD latency perfectly.  Even a 2ms error per chunk creates
      a ~18ms residual temporal spread across the screen, leaving faint bands.

      With live rendering, each chunk's content is computed the instant before
      it is written to GRAM.  The actual per-chunk delay — whatever it is — is
      automatically absorbed because the timestamp used IS the send timestamp.
      Result: the displayed animation is perfectly temporally consistent, with
      at most one single-row glitch at the frame boundary (the dithered tear).

    Returns None if numpy is unavailable; caller falls back to test_frames().
    """
    if not HAS_NUMPY:
        return None

    import colorsys
    palette = [tuple(int(c * 255) for c in colorsys.hsv_to_rgb(i / 256.0, 1.0, 1.0))
               for i in range(256)]
    pal_np = np.array(palette, dtype=np.uint8)        # (256, 3)
    x_frac = np.arange(LCD_W, dtype=np.float32)[np.newaxis, :] / LCD_W  # (1, W)

    t0 = time.monotonic()

    def gen(ci, frame_t=None):
        """Render chunk ci as BGR565 bytes.

        frame_t — the shared wall-clock time for this frame.  When all chunks
        receive the same frame_t the colour at the chunk boundary is
        continuous (differs only by the natural y-gradient, ~1 palette step),
        making the seam invisible.  Defaults to time.monotonic() so the
        function still works standalone.

        Rolling-shutter compensation
        ─────────────────────────────
        Chunks arrive at GRAM sequentially, ~T_frame apart.  During the gap
        the display shows new chunk-1 (top) alongside old chunk-0 (bottom).
        With a gently-sloped diagonal (K_Y = +0.3) the two halves appear
        spatially misaligned by ~35 rows (~1 cm) because old content is one
        frame period behind in time.

        Fix: choose K_Y = -(LCD_H × T_frame × speed) = -(80 × 0.135 × 1.0)
        ≈ -10.8.  This makes the between-row colour step K_Y/LCD_H exactly
        equal to the one-frame temporal step, so old-chunk-0 row 39 and
        new-chunk-1 row 40 show the same colour during the transition.  In
        the stable window the chunk boundary has exactly the same per-row step
        as all other row pairs — visually indistinguishable.

        Sensitivity: ±15 ms frame-period error → ±0.0019 cycle residual seam
        (≈ 0.5 palette steps) — effectively invisible.
        """
        if frame_t is None:
            frame_t = time.monotonic()
        row0 = ci * CHUNK_ROWS
        # K_Y = -(LCD_H × T_frame × speed)  — rolling-shutter compensation slope.
        # Negative coefficient: stripes cycle downward (opposite to K_Y > 0),
        # calibrated so the temporal offset cancels the spatial offset at the
        # chunk boundary.  Adjust K_Y if frame rate changes significantly.
        K_Y = -10.8   # = -(80 × 0.135 s/frame × 1.0 cycle/s)
        y_frac = (np.arange(row0, row0 + CHUNK_ROWS, dtype=np.float32)
                  / LCD_H * K_Y)[:, np.newaxis]        # (CHUNK_ROWS, 1)
        t = frame_t - t0
        h_idx = ((x_frac + y_frac + t) % 1.0 * 256).astype(np.int32) % 256
        rgb = pal_np[h_idx]                             # (CHUNK_ROWS, W, 3)
        r = rgb[:, :, 0].astype(np.uint16)
        g = rgb[:, :, 1].astype(np.uint16)
        b = rgb[:, :, 2].astype(np.uint16)
        bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
        return bgr565.astype('<u2').tobytes()

    return gen


def screen_frames(bbox):
    x, y, w, h = bbox
    if HAS_MSS:
        with mss.mss() as sct:
            region = {"left": x, "top": y, "width": w, "height": h}
            while True:
                shot = sct.grab(region)
                img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                yield img
    else:
        while True:
            img = ImageGrab.grab(bbox=(x, y, x + w, y + h))
            yield img


def window_frames(title):
    try:
        import win32gui, win32ui
        from ctypes import windll
        import ctypes
    except ImportError:
        print("ERROR: pywin32 not installed. Run: pip install pywin32")
        sys.exit(1)

    # Make this process DPI-aware so GetClientRect returns physical pixel
    # dimensions and PrintWindow renders into a correctly-sized bitmap.
    # Without this, on high-DPI monitors GetClientRect returns logical coords
    # but PrintWindow renders at physical resolution — only the upper-left
    # portion of the game fills the bitmap and the rest is clipped.
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()   # fallback: system DPI aware
        except Exception:
            pass

    def find_window(partial_title):
        result = []
        def cb(hwnd, _):
            if win32gui.IsWindowVisible(hwnd):
                t = win32gui.GetWindowText(hwnd)
                if partial_title.lower() in t.lower():
                    result.append(hwnd)
        win32gui.EnumWindows(cb, None)
        return result[0] if result else None

    hwnd = find_window(title)
    if not hwnd:
        print(f"ERROR: No visible window matching '{title}'")
        sys.exit(1)
    print(f"  Capturing window: '{win32gui.GetWindowText(hwnd)}'")

    while True:
        try:
            # GetClientRect returns client-area dimensions in physical pixels
            # (with DPI awareness set above).  This excludes the title bar and
            # window borders, so we capture only the game content.
            cl, ct, cr, cb_r = win32gui.GetClientRect(hwnd)
            w, h = cr - cl, cb_r - ct
            if w == 0 or h == 0:
                time.sleep(0.05)
                continue
            hwnd_dc  = win32gui.GetWindowDC(hwnd)
            mfc_dc   = win32ui.CreateDCFromHandle(hwnd_dc)
            save_dc  = mfc_dc.CreateCompatibleDC()
            bmp      = win32ui.CreateBitmap()
            bmp.CreateCompatibleBitmap(mfc_dc, w, h)
            save_dc.SelectObject(bmp)
            # PW_CLIENTONLY (1) | PW_RENDERFULLCONTENT (2) = 3
            windll.user32.PrintWindow(hwnd, save_dc.GetSafeHdc(), 3)
            bmpinfo  = bmp.GetInfo()
            bmpstr   = bmp.GetBitmapBits(True)
            img = Image.frombuffer("RGB", (bmpinfo["bmWidth"], bmpinfo["bmHeight"]),
                                   bmpstr, "raw", "BGRX", 0, 1)
            win32gui.DeleteObject(bmp.GetHandle())
            save_dc.DeleteDC(); mfc_dc.DeleteDC()
            win32gui.ReleaseDC(hwnd, hwnd_dc)
            yield img
        except Exception as e:
            print(f"\n  Window capture error: {e}")
            time.sleep(0.1)


def file_frames(path):
    frame_size = LCD_W * LCD_H * 3
    last_mtime = 0
    last_img = None
    while True:
        try:
            mtime = os.path.getmtime(path)
            if mtime != last_mtime:
                with open(path, 'rb') as f:
                    raw = f.read(frame_size)
                if len(raw) == frame_size:
                    last_img = Image.frombytes('RGB', (LCD_W, LCD_H), raw)
                    last_mtime = mtime
            if last_img:
                yield last_img
            else:
                time.sleep(0.05)
        except FileNotFoundError:
            time.sleep(0.1)


def video_frames(path):
    import subprocess as sp
    cmd = ["ffmpeg", "-i", path,
           "-vf", f"scale={LCD_W}:{LCD_H}",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = sp.Popen(cmd, stdout=sp.PIPE, stderr=sp.DEVNULL)
    frame_bytes = LCD_W * LCD_H * 3
    while True:
        raw = proc.stdout.read(frame_bytes)
        if len(raw) < frame_bytes:
            break
        yield Image.frombytes("RGB", (LCD_W, LCD_H), raw)


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Stream frames to vape display via SWD")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--test",   action="store_true",
                      help="Animated rainbow test pattern")
    mode.add_argument("--screen", nargs="*", metavar=("X","Y","W","H"),
                      help="Capture screen region [x y w h] (default: full monitor)")
    mode.add_argument("--window", metavar="TITLE",
                      help="Capture window by title substring (Windows only)")
    mode.add_argument("--video",  metavar="FILE",
                      help="Stream from video file (requires ffmpeg)")
    mode.add_argument("--file",   metavar="PATH",
                      help="Read raw RGB24 frames from file (128x160x3 bytes)")
    mode.add_argument("--halt",   action="store_true",
                      help="Turn off the LCD and halt the MCU")

    ap.add_argument("--freq", type=int, default=8000000,
                    help="SWD frequency Hz (default 8000000; fall back to 4000000 if unstable)")
    ap.add_argument("--native", action="store_true",
                    help="Use Windows-native OpenOCD (no WSL/sidecar) — faster, ~3-4ms/chunk")
    ap.add_argument("--wsl", action="store_true",
                    help="Force WSL+OpenOCD backend even on Windows (legacy)")
    args = ap.parse_args()

    if not HAS_PIL and not args.halt:
        print("ERROR: pip install pillow")
        sys.exit(1)

    # Auto-select native mode when Windows OpenOCD is present and --wsl not forced
    use_native = args.native or (
        os.path.isfile(OCD_WIN_EXE) and not args.wsl
    )

    if use_native:
        print(f"Connecting to N32G031 at {args.freq // 1000} kHz via Windows OpenOCD (native)…")
        disp = VapeDisplayNative(freq=args.freq)
    else:
        print(f"Connecting to N32G031 at {args.freq // 1000} kHz via OpenOCD/WSL…")
        disp = VapeDisplay(freq=args.freq)

    if args.halt:
        print("Turning off LCD and halting MCU…")
        disp.sleep_and_halt()
        disp.close()
        return

    # Pick frame generator (test mode handled separately below with live chunks)
    frames = None
    if args.test:
        pass  # handled by send_frame_live below
    elif args.screen is not None:
        if args.screen and len(args.screen) == 4:
            bbox = tuple(int(v) for v in args.screen)
        else:
            if HAS_MSS:
                with mss.mss() as sct:
                    m = sct.monitors[1]
                    bbox = (m["left"], m["top"], m["width"], m["height"])
            else:
                bbox = (0, 0, 1920, 1080)
        print(f"  Screen capture region: {bbox}")
        frames = screen_frames(bbox)
    elif args.window:
        frames = window_frames(args.window)
    elif args.file:
        frames = file_frames(args.file)
    elif args.video:
        frames = video_frames(args.video)

    print("Connected. Streaming… (Ctrl-C to stop)\n")

    # Phase-dither constant: one display scan period ≈ 16.67ms (60Hz).
    # At ~8fps the frame period is ~125ms = 7.5 × 16.67ms — a perfect 2-frame
    # phase lock that makes the display scan tear the top half on even frames
    # and the bottom half on odd frames (the "alternating blocks" artifact).
    # Adding a uniform random sleep of [0, DITHER_S] each frame spreads the
    # scan phase uniformly over the display cycle, so the tear position is
    # random every frame instead of locked.  The cost is ~DITHER_S/2 average
    # latency (~8ms), negligible at these frame rates.
    DITHER_S = 0.0167   # one display frame period

    frame_idx = 0
    total_time = 0.0

    def _print_stats(elapsed, sent):
        nonlocal frame_idx, total_time
        total_time += elapsed
        fps = 1.0 / elapsed if elapsed > 0 else 0
        avg_fps = frame_idx / total_time if total_time > 0 else 0
        print(f"\r  frame {frame_idx:4d}  {fps:5.1f} fps  avg {avg_fps:5.1f} fps  "
              f"sent {sent}/{NUM_CHUNKS}  elapsed {elapsed*1000:.0f}ms  ",
              end="", flush=True)
        frame_idx += 1

    try:
        if args.test:
            # ── Live per-chunk test mode ─────────────────────────────────────
            # gen(ci) is called immediately before _send_chunk(ci), so it uses
            # the exact wall-clock time that chunk will hit GRAM.  No fixed
            # CHUNK_DELAY_S estimate needed; compensation is always perfect.
            chunk_gen = test_chunk_gen()
            if chunk_gen is None:
                # numpy unavailable — fall back to PIL loop
                frames = test_frames()
            else:
                while True:
                    elapsed, sent = disp.send_frame_live(chunk_gen)
                    time.sleep(random.uniform(0.0, DITHER_S))
                    _print_stats(elapsed, sent)

        if frames is not None:
            # ── PIL-image frame loop (screen / window / video / numpy fallback) ──
            for img in frames:
                chunks = image_to_chunks(img)
                elapsed, sent = disp.send_frame(chunks)
                time.sleep(random.uniform(0.0, DITHER_S))
                _print_stats(elapsed, sent)

    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        disp.close()


if __name__ == "__main__":
    main()
