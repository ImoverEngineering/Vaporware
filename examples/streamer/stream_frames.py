#!/usr/bin/env python3
"""stream_frames.py — Stream frames to the N32G031 vape display via SWD.

Requires: pip install pillow mss pywin32
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
    Double-buffer layout — two independent 64x8 logical ping-pong buffers,
    MCU 2x-scales each to 128x16 display pixels.

    CTRL   @ 0x20000010   0xDEAD0001 = halt, 0xDEAD0000 = reset

    IDX_A  @ 0x20000100   chunk index 0-9
    BUF_A  @ 0x20000104   1024 bytes pixel data (64x8 px logical, BGR565 LE)
    TRIG_A @ 0x20000504   0xCC written LAST — MCU blits A (scaled 2x), clears to 0

    IDX_B  @ 0x20000508   chunk index 0-9
    BUF_B  @ 0x2000050C   1024 bytes pixel data (64x8 px logical, BGR565 LE)
    TRIG_B @ 0x2000090C   0xCC written LAST — MCU blits B (scaled 2x), clears to 0

    PC pipelines all dirty chunk writes via write_memory inline (no file I/O,
    no sidecar, no per-chunk round-trips). All commands fire before any prompt
    is collected. MCU draw time at 4 MHz SPI = 8.2 ms per chunk; SWD write per
    chunk = ~11 ms; A/B alternation means a buffer isn't reused for 22 ms —
    always after MCU finishes drawing (8.2 ms). Safe without explicit polling.
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

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

# ── Protocol ──────────────────────────────────────────────────────────────────
CTRL_ADDR  = 0x20000010   # legacy: reset / sleep

# Half-resolution double-buffer: 64×80 logical pixels, 2× scaled by MCU to 128×160.
# 4× fewer bytes per frame vs full-res → ~4× more fps for all-dirty frames.
#
# Double-buffer A  (1024 B pixel data)
BUF_A_IDX_ADDR  = 0x20000100
BUF_A_DATA_ADDR = 0x20000104   # IDX_A(4) + 1024 bytes pixel data
BUF_A_TRIG_ADDR = 0x20000504   # IDX_A(4) + BUF_A(1024) past BUF_A_IDX_ADDR

# Double-buffer B  (1024 B pixel data)
BUF_B_IDX_ADDR  = 0x20000508
BUF_B_DATA_ADDR = 0x2000050C   # IDX_B(4) + 1024 bytes pixel data
BUF_B_TRIG_ADDR = 0x2000090C   # IDX_B(4) + BUF_B(1024) past BUF_B_IDX_ADDR

# Combined packet sizes:
#   Packet A/B: IDX(4) + BUF(1024) + TRIG(4) = 1032 B
#   At 91 KB/s: ~11 ms per full chunk; 10 chunks all dirty → ~113 ms → ~9 fps
#   MCU draw_chunk_2x at 24 MHz SPI: ~1.4 ms → always done before PC revisits
BUF_CHUNK_BYTES = 1024    # pixel bytes per chunk (64 x 8 x 2)
FULL_PACKET     = 1032    # IDX(4) + BUF(1024) + TRIG(4)

# At 4 MHz SPI each 128×16 blit = 4096 bytes = 8.19 ms.
# Worst-case MCU latency before starting a draw: up to 8 ms (if it is mid-draw
# on the *other* buffer when we set TRIG).  Total safe window = 8 + 8.2 + 0.3 = 16.5 ms.
# send_frame() uses this as the threshold for skipping the poll-for-clear check.
MCU_DRAW_S = 0.0165      # 16.5 ms worst-case: 8 ms MCU latency + 8.2 ms SPI + margin

CTRL_IDLE  = 0x00000000
CTRL_CHUNK = 0x000000CC
CTRL_RESET = 0xDEAD0000
CTRL_SLEEP = 0xDEAD0001

CHUNK_ROWS  = 8     # logical rows per chunk
CHUNK_W     = 64    # logical pixels per row (MCU 2× scales to 128)
NUM_CHUNKS  = 10    # 10 × 8 logical rows = 80 → 160 display rows
LCD_W, LCD_H = 128, 160

# ── OpenOCD / WSL config ──────────────────────────────────────────────────────
# Use the local custom cfg — it has no stm32f1x flash bank and no bad examine-end
# hook (which writes to 0x40007030, an invalid address on N32G031 that makes
# OpenOCD exit rc=1 before port 4444 ever opens).
# We pass -c "init" -c "reset halt" explicitly; the cfg does NOT call them itself.
OCD_TARGET_CFG = (
    "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/streamer/"
    "n32g031.openocd.cfg"
)
USBIPD_BUSID = "1-2"
OCD_TELNET_PORT = 4444


# ── Color conversion ──────────────────────────────────────────────────────────
def image_to_chunks(img):
    """Convert any PIL Image → NUM_CHUNKS raw chunk byte-strings (BGR565 LE).

    Image is downscaled to CHUNK_W × (NUM_CHUNKS*CHUNK_ROWS) logical pixels.
    The MCU 2× scales each chunk back to full display resolution via
    display_draw_chunk_2x(), so 64×80 logical → 128×160 on-screen.

    Uses numpy for vectorised BGR565 conversion (~50× faster than a Python loop).
    Falls back to a pure-Python loop if numpy is not available.
    """
    log_h = NUM_CHUNKS * CHUNK_ROWS   # 80 logical rows
    # BILINEAR is 3-5× faster than LANCZOS and indistinguishable at 64×80
    img = img.convert("RGB").resize((CHUNK_W, log_h), Image.BILINEAR)

    if HAS_NUMPY:
        arr = np.frombuffer(img.tobytes(), dtype=np.uint8).reshape(log_h, CHUNK_W, 3)
        r = arr[:, :, 0].astype(np.uint16)
        g = arr[:, :, 1].astype(np.uint16)
        b = arr[:, :, 2].astype(np.uint16)
        # BGR565 little-endian: bits [15:11]=B [10:5]=G [4:0]=R
        bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
        raw = bgr565.astype('<u2').tobytes()   # already LE on x86
        chunk_bytes = CHUNK_W * CHUNK_ROWS * 2
        return [raw[ci * chunk_bytes:(ci + 1) * chunk_bytes]
                for ci in range(NUM_CHUNKS)]

    # Pure-Python fallback
    data = img.tobytes()
    chunks = []
    for ci in range(NUM_CHUNKS):
        row0 = ci * CHUNK_ROWS
        buf = bytearray(CHUNK_W * CHUNK_ROWS * 2)
        off = 0
        for row in range(CHUNK_ROWS):
            src = ((row0 + row) * CHUNK_W) * 3
            for col in range(CHUNK_W):
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
    # native filesystem.  Reading /tmp from WSL is instant (tmpfs); reading
    # /mnt/c/ (Windows filesystem) adds ~50 ms of cross-FS overhead per chunk.
    #
    # For full packets: 2056 bytes [IDX(4)][BUF(2048)][TRIG(4)].
    # For sparse writes: variable size, one range at a time.
    # The file is overwritten for each range; prompts are collected between writes
    # to ensure OpenOCD has read the file before we clobber it.
    _WSL_CHUNK_PATH = '/tmp/vape_chunk.bin'

    def __init__(self, freq=4000000):
        # Per-buffer previous-frame tracking for trimmed/sparse writes.
        # _prev_A[i] = bytes last written to BUF_A for chunk index i (or None).
        # _prev_B[i] = bytes last written to BUF_B for chunk index i (or None).
        # We alternate A/B for consecutive dirty chunks each frame; knowing which
        # buffer a chunk index last occupied lets us diff against the correct SRAM
        # content so partial writes leave the correct pixels in the other region.
        self._prev_A = [None] * NUM_CHUNKS   # bytes in SRAM buffer A for each chunk
        self._prev_B = [None] * NUM_CHUNKS   # bytes in SRAM buffer B for each chunk
        self._last_sent = [None] * NUM_CHUNKS # content last triggered → what display shows
        self._buf_toggle = False   # False → next dirty chunk goes to A; True → B
        # Timestamps of the last TRIG write for each buffer.  Used to enforce the
        # MCU_DRAW_S guard in send_frame(): we must not overwrite BUF_A/B while
        # the MCU is still SPI-blitting from it.  Initialised to 0 so the first
        # write to each buffer always proceeds without sleeping.
        self._last_trig_A_time = 0.0
        self._last_trig_B_time = 0.0
        self._freq_khz = freq // 1000
        self._sock = None
        self._ocd_proc = None
        self._wsl_keepalive = None  # persistent WSL process; keeps WSL VM alive so usbipd doesn't detach
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

        # Attach ST-Link to WSL
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
        # Firmware runs: clock_init → clock_boost_48mhz (PLL, ~1 ms) →
        # display_init at 24 MHz SPI (~400 ms) → draw_waiting → streaming loop.
        # OpenOCD briefly loses SWD when PLL switches; auto-reconnects in <100 ms.
        # 1.5 s is conservative margin for display_init to fully complete.
        print("  Resuming firmware (waiting for display init ~1.5 s)…")
        self._cmd('resume')
        time.sleep(1.5)

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

    def _wait_trig_clear(self, trig_addr, timeout_s=0.5):
        """Poll TRIG via mrw until MCU clears it (it finished drawing from that buffer).

        Fast path: returns on the first poll if TRIG is already 0 — cost is one
        ~2 ms telnet round-trip.  Only spins when the timing guard says the MCU
        might still be drawing (elapsed < MCU_DRAW_S).
        """
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            resp = self._cmd(f'mrw 0x{trig_addr:08X}')
            m = re.search(r'0x([0-9a-fA-F]+)', resp)
            if m and int(m.group(1), 16) == 0:
                return
            time.sleep(0.001)

    @staticmethod
    def _compute_ranges(chunk, prev_chunk, gap_thr=180):
        """Return list of (start, end) dirty byte ranges, merging gaps <= gap_thr bytes.

        Scans word-by-word (4 bytes).  Contiguous or near-contiguous dirty words
        are merged into a single range if the gap between them is <= gap_thr bytes.

        Break-even for splitting vs merging one gap:
            USB throughput ~91 KB/s; one extra load_image round-trip ~2 ms.
            2 ms x 91 KB/s = 182 bytes — so merging gaps <= 180 bytes saves a
            round-trip's worth of overhead.
        """
        WORD = 4
        n = len(chunk)
        ranges = []
        cur_start = None
        last_end  = 0
        for i in range(0, n, WORD):
            if chunk[i:i+WORD] != prev_chunk[i:i+WORD]:
                if cur_start is None:
                    cur_start = i
                    last_end  = i + WORD
                elif i - last_end <= gap_thr:
                    last_end = i + WORD   # extend current range through the gap
                else:
                    ranges.append((cur_start, last_end))
                    cur_start = i
                    last_end  = i + WORD
        if cur_start is not None:
            ranges.append((cur_start, last_end))
        return ranges

    def _queue_chunk(self, idx, chunk, buf_idx_addr, buf_data_addr, buf_trig_addr,
                     prev_chunk=None):
        """Queue a chunk write via inline write_memory — no file I/O, no sidecar.

        Sends all OpenOCD commands to the socket without collecting any prompts.
        Caller must drain exactly the returned number of pending prompts afterward.

        All dirty data is encoded as hex words and sent inline in the telnet command,
        so there is no cross-process IPC and no WSL filesystem overhead.

        Buffer safety: caller pipelines A and B alternately; each buffer is reused
        only after 2 SWD write slots (~22 ms), which is always after the MCU
        finishes drawing from it (~8.2 ms at 4 MHz SPI). No TRIG polling needed.

        Returns the number of OpenOCD commands queued (= prompts to collect).
        """
        # ── Dirty-range detection ─────────────────────────────────────────────
        if prev_chunk is not None and len(prev_chunk) == len(chunk):
            ranges = self._compute_ranges(chunk, prev_chunk)
            if not ranges:
                return 0   # identical — nothing to send
        else:
            ranges = [(0, len(chunk))]

        total_dirty = sum(e - s for s, e in ranges)

        # ── Full combined write_memory ─────────────────────────────────────────
        # One call: [IDX(4)][BUF(1024)][TRIG(4)] = 258 words.
        # TRIG is the last word, so MCU cannot fire until BUF is fully written.
        if total_dirty >= FULL_PACKET - 364:
            packet = struct.pack('<I', idx) + chunk + struct.pack('<I', CTRL_CHUNK)
            words = struct.unpack(f'<{len(packet)//4}I', packet)
            hw = ' '.join(f'0x{w:08X}' for w in words)
            self._sock.sendall(
                f'write_memory 0x{buf_idx_addr:08X} 32 {{{hw}}}\n'.encode()
            )
            return 1

        # ── Sparse write_memory (few dirty ranges) ────────────────────────────
        # IDX first, then each dirty range, then TRIG last.
        pending = 0
        self._sock.sendall(f'mww 0x{buf_idx_addr:08X} 0x{idx:08X}\n'.encode())
        pending += 1

        for s, e in ranges:
            buf = chunk[s:e]
            if len(buf) % 4:
                buf = buf + b'\xff' * (4 - len(buf) % 4)
            words = struct.unpack(f'<{len(buf)//4}I', buf)
            hw = ' '.join(f'0x{w:08X}' for w in words)
            self._sock.sendall(
                f'write_memory 0x{buf_data_addr + s:08X} 32 {{{hw}}}\n'.encode()
            )
            pending += 1

        self._sock.sendall(
            f'mww 0x{buf_trig_addr:08X} 0x{CTRL_CHUNK:08X}\n'.encode()
        )
        pending += 1
        return pending

    def send_frame(self, chunks):
        """Send all changed chunks, pipelining all writes before collecting prompts.

        All dirty-chunk write_memory commands are fired in one burst before any
        response is awaited. OpenOCD executes them sequentially over SWD while
        Python is already collecting the returned prompts from the socket.

        Per-buffer timing safety (no TRIG polling needed):
          SWD write per chunk ≈ 11 ms; MCU draw per chunk ≈ 8.2 ms.
          Buffer A is reused at chunk +2 (≥22 ms later); MCU finished at ≤19.2 ms.
          Buffer B: same argument. Both buffers are always clear before reuse.

        _last_sent[ci] — what is currently displayed for chunk ci.
        _prev_A/B[ci]  — what is in each SRAM buffer for chunk ci (for sparse diff).
        """
        t0 = time.monotonic()
        total_pending = 0
        chunks_sent = 0

        for ci, chunk in enumerate(chunks):
            if chunk == self._last_sent[ci]:
                continue   # display already shows this content

            if not self._buf_toggle:
                n = self._queue_chunk(ci, chunk,
                                      BUF_A_IDX_ADDR, BUF_A_DATA_ADDR, BUF_A_TRIG_ADDR,
                                      self._prev_A[ci])
                self._prev_A[ci] = chunk
            else:
                n = self._queue_chunk(ci, chunk,
                                      BUF_B_IDX_ADDR, BUF_B_DATA_ADDR, BUF_B_TRIG_ADDR,
                                      self._prev_B[ci])
                self._prev_B[ci] = chunk

            total_pending += n
            self._last_sent[ci] = chunk
            self._buf_toggle = not self._buf_toggle
            chunks_sent += 1

        # Collect all queued prompts in one pass — by now most SWD writes are
        # already complete or in flight, so these return quickly.
        for _ in range(total_pending):
            self._read_prompt()

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
        self._prev_A = [None] * NUM_CHUNKS
        self._prev_B = [None] * NUM_CHUNKS
        self._last_sent = [None] * NUM_CHUNKS
        self._buf_toggle = False
        self._last_trig_A_time = 0.0
        self._last_trig_B_time = 0.0

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
        if self._wsl_keepalive:
            try:
                self._wsl_keepalive.terminate()
            except Exception:
                pass


# ── Frame providers ───────────────────────────────────────────────────────────
def test_frames():
    """Animated rainbow — one full hue cycle per second using wall-clock time."""
    import colorsys
    # Pre-build the palette once (256 entries) and animate by rotating the offset.
    # This is ~50× faster than calling hsv_to_rgb per pixel per frame.
    palette = [tuple(int(c * 255) for c in colorsys.hsv_to_rgb(i / 256.0, 1.0, 1.0))
               for i in range(256)]

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


def screen_frames(bbox):
    x, y, w, h = bbox
    if HAS_MSS:
        with mss.MSS() as sct:
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
    """Capture a window by title using mss (fast screen-region capture).

    Uses win32gui to find the window and get its bounds, then mss to capture
    the screen region.  mss grabs directly from the display buffer (~5 ms) vs
    PrintWindow's GDI blit (~1500 ms), giving ~300× faster capture.
    Works with SDL2/OpenGL windows that PrintWindow cannot capture correctly.
    """
    try:
        import win32gui
    except ImportError:
        print("ERROR: pywin32 not installed. Run: pip install pywin32")
        sys.exit(1)
    if not HAS_MSS:
        print("ERROR: mss not installed. Run: pip install mss")
        sys.exit(1)

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

    with mss.MSS() as sct:
        while True:
            try:
                left, top, right, bot = win32gui.GetWindowRect(hwnd)
                region = {"left": left, "top": top,
                          "width": right - left, "height": bot - top}
                shot = sct.grab(region)
                img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                yield img
            except Exception:
                # SDL2 games recreate their window handle on mode switches.
                new_hwnd = find_window(title)
                if new_hwnd and new_hwnd != hwnd:
                    hwnd = new_hwnd
                    print(f"\n  Window handle refreshed: '{win32gui.GetWindowText(hwnd)}'")
                else:
                    time.sleep(0.05)


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
    mode.add_argument("--screen", nargs="*", metavar="X Y W H",
                      help="Capture screen region [x y w h] (default: full monitor)")
    mode.add_argument("--window", metavar="TITLE",
                      help="Capture window by title substring (Windows only)")
    mode.add_argument("--video",  metavar="FILE",
                      help="Stream from video file (requires ffmpeg)")
    mode.add_argument("--file",   metavar="PATH",
                      help="Read raw RGB24 frames from file (128x160x3 bytes)")
    mode.add_argument("--halt",   action="store_true",
                      help="Turn off the LCD and halt the MCU")

    ap.add_argument("--freq", type=int, default=4000000,
                    help="SWD frequency Hz (default 4000000; try 8000000 with ST-Link/v3)")
    args = ap.parse_args()

    if not HAS_PIL and not args.halt:
        print("ERROR: pip install pillow")
        sys.exit(1)

    print(f"Connecting to N32G031 at {args.freq // 1000} kHz via OpenOCD/WSL…")
    disp = VapeDisplay(freq=args.freq)

    if args.halt:
        print("Turning off LCD and halting MCU…")
        disp.sleep_and_halt()
        disp.close()
        return

    # Pick frame generator
    if args.test:
        frames = test_frames()
    elif args.screen is not None:
        if args.screen and len(args.screen) == 4:
            bbox = tuple(int(v) for v in args.screen)
        else:
            if HAS_MSS:
                with mss.MSS() as sct:
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

    frame_idx = 0
    total_time = 0.0
    try:
        for img in frames:
            chunks = image_to_chunks(img)
            elapsed, sent = disp.send_frame(chunks)
            total_time += elapsed
            fps = 1.0 / elapsed if elapsed > 0 else 0
            avg_fps = frame_idx / total_time if total_time > 0 else 0
            print(f"\r  frame {frame_idx:4d}  {fps:5.1f} fps  avg {avg_fps:5.1f} fps  "
                  f"sent {sent}/{NUM_CHUNKS}  elapsed {elapsed*1000:.0f}ms  ",
                  end="", flush=True)
            frame_idx += 1
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        disp.close()


if __name__ == "__main__":
    main()
