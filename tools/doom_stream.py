#!/usr/bin/env python3
"""
doom_stream.py — Host-side SWD mailbox driver for GC9107 streaming firmware.

Usage:
    python doom_stream.py test       — color-fill test (verifies protocol)
    python doom_stream.py stream     — stream Doom frames (requires pydoom)
    python doom_stream.py screen     — stream the screen crop as a live feed

Requires: OpenOCD running with telnet port 4444 on localhost.
Start OpenOCD in a separate terminal:
    wsl openocd -f /mnt/c/.../n32g031.openocd.cfg \
        -c "n32g031.cpu configure -defer-examine" \
        -c "init; reset halt; n32g031.cpu arp_examine; resume"

Protocol (see main.c for full spec):
    Mailbox at 0x20001000:
      +0x000  cmd   uint32   0=idle 1=fill 2=start 3=chunk 4=end
      +0x004  x0    uint16   window
      +0x006  y0    uint16
      +0x008  x1    uint16
      +0x00A  y1    uint16
      +0x00C  npix  uint16   # pixels in pixels[]
      +0x00E  _pad  uint16
      +0x010  pixels uint16[1024]  RGB565 pixel data
"""

import socket, struct, time, sys, array
import subprocess, threading

# ── Display constants ────────────────────────────────────────────────
LCD_W, LCD_H = 128, 160
CHUNK_PIX    = 1280                  # pixels per SWD write; must be <= 2040 (SRAM limit)
#                                    # NOTE: firmware declares pixels[1024] but blit(n) is a
#                                    # simple loop over mb->pixels[0..n-1]; since 1280 > 1024
#                                    # the extra 256 pixels live in the free SRAM just past
#                                    # the mailbox struct (0x20001810+), which is safe to use.
#                                    # Choosing 1280 (16 chunks/frame) minimises per-chunk overhead.
CHUNK_BYTES  = CHUNK_PIX * 2        # bytes per chunk

# ── Mailbox SRAM addresses ───────────────────────────────────────────
MB_BASE      = 0x20001000
MB_CMD       = MB_BASE + 0x000
MB_X0        = MB_BASE + 0x004
MB_Y0        = MB_BASE + 0x006
MB_X1        = MB_BASE + 0x008
MB_Y1        = MB_BASE + 0x00A
MB_NPIX      = MB_BASE + 0x00C
MB_PIXELS    = MB_BASE + 0x010

# ── Commands ─────────────────────────────────────────────────────────
CMD_IDLE        = 0
CMD_FILL        = 1
CMD_FRAME_START = 2
CMD_FRAME_CHUNK = 3
CMD_FRAME_END   = 4

# ── Color helpers (BGR-swapped display, MADCTL=0x98) ─────────────────
def rgb565(r, g, b):
    """Convert 8-bit RGB to RGB565 with B/R swap for GC9107 MADCTL=0x98."""
    return ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)

COL_BLACK  = 0x0000
COL_WHITE  = 0xFFFF
COL_RED    = rgb565(255,   0,   0)
COL_GREEN  = rgb565(  0, 255,   0)
COL_BLUE   = rgb565(  0,   0, 255)

# ════════════════════════════════════════════════════════════════════
# OpenOCD telnet interface
# ════════════════════════════════════════════════════════════════════

class OpenOCD:
    """Thin wrapper around the OpenOCD TCL server (port 6666).

    The TCL server uses SUB (0x1A) as the command/response delimiter,
    which lets us do clean request/response framing.  The telnet port
    (4444) uses a human-readable prompt and does NOT use 0x1A framing.
    """

    def __init__(self, host='localhost', port=6666, timeout=5.0):
        self.s = socket.socket()
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # disable Nagle
        self.s.settimeout(timeout)
        self.s.connect((host, port))
        # TCL port has no banner — nothing to drain
        # Bump adapter speed to 4 MHz for faster SWD block writes
        self.cmd('adapter speed 4000')

    def _drain(self, wait=0.05):
        """Read and discard whatever is in the socket buffer."""
        self.s.settimeout(wait)
        try:
            while self.s.recv(4096):
                pass
        except socket.timeout:
            pass
        finally:
            self.s.settimeout(5.0)

    def cmd(self, tcl, drain=True):
        """Send a TCL command and return the response string."""
        self.s.sendall((tcl + '\x1a').encode())
        buf = b''
        self.s.settimeout(5.0)
        while not buf.endswith(b'\x1a'):
            buf += self.s.recv(4096)
        return buf[:-1].decode(errors='replace').strip()

    def mdw(self, addr):
        """Read one 32-bit word from target memory."""
        resp = self.cmd(f'mdw 0x{addr:08X}')
        # resp looks like "0x20001000: deadbeef "
        return int(resp.split(':')[1].strip().split()[0], 16)

    def mdh(self, addr):
        """Read one 16-bit halfword."""
        resp = self.cmd(f'mdh 0x{addr:08X}')
        return int(resp.split(':')[1].strip().split()[0], 16)

    def mww(self, addr, val):
        """Write one 32-bit word."""
        self.cmd(f'mww 0x{addr:08X} 0x{val:08X}')

    def mwh(self, addr, val):
        """Write one 16-bit halfword."""
        self.cmd(f'mwh 0x{addr:08X} 0x{val:04X}')

    def write_block(self, addr, data: bytes):
        """Write a byte buffer to target SRAM using write_memory.
        data must be a multiple of 4 bytes (pad with zeros if needed)."""
        if len(data) % 4:
            data += b'\x00' * (4 - len(data) % 4)
        words = struct.unpack(f'<{len(data)//4}I', data)
        hex_words = ' '.join(f'0x{w:08X}' for w in words)
        self.cmd(f'write_memory 0x{addr:08X} 32 {{ {hex_words} }}')

    def close(self):
        self.s.close()


# ════════════════════════════════════════════════════════════════════
# Mailbox high-level API
# ════════════════════════════════════════════════════════════════════

class Display:
    """High-level display driver using the SWD mailbox."""

    def __init__(self, ocd: OpenOCD, verbose=False):
        self.ocd = ocd
        self.verbose = verbose
        self._wait_ready(timeout=5.0)

    def _wait_ready(self, timeout=5.0):
        """Block until the MCU clears cmd to CMD_IDLE (firmware started)."""
        t0 = time.time()
        while True:
            val = self.ocd.mdw(MB_CMD)
            if val == CMD_IDLE:
                return
            if time.time() - t0 > timeout:
                raise TimeoutError(f'MCU mailbox not ready after {timeout}s (cmd=0x{val:08X})')
            time.sleep(0.01)

    def _wait_idle(self, timeout=2.0):
        """Poll MB_CMD until it returns CMD_IDLE (MCU finished command).

        Each mdw call takes ~8 ms at 4 MHz SWD, while the MCU processes
        1024 pixels via SPI in ~4 ms.  We poll immediately (no sleep) —
        the first mdw read will almost always return CMD_IDLE already.
        """
        t0 = time.time()
        while True:
            val = self.ocd.mdw(MB_CMD)
            if val == CMD_IDLE:
                return
            if time.time() - t0 > timeout:
                raise TimeoutError('MCU did not clear cmd in time')
            # No sleep: each mdw takes ~8ms which is already > MCU blit time

    def fill(self, color: int):
        """Fill entire screen with a single RGB565 color."""
        self.ocd.write_block(MB_PIXELS, struct.pack('<H', color))
        self.ocd.mww(MB_NPIX, 1)
        self.ocd.mww(MB_CMD, CMD_FILL)
        self._wait_idle()

    def blit_frame(self, pixels: bytes,
                   x0=0, y0=0, x1=LCD_W-1, y1=LCD_H-1):
        """Stream a full frame of RGB565 pixel data to the display.

        pixels: bytes object, len = (x1-x0+1)*(y1-y0+1)*2, big-endian pairs.
        The data is split into CHUNK_PIX-pixel chunks and sent via the
        CMD_FRAME_START / CMD_FRAME_CHUNK / CMD_FRAME_END sequence.
        """
        npix_total = (x1 - x0 + 1) * (y1 - y0 + 1)
        if len(pixels) < npix_total * 2:
            raise ValueError(f'Need {npix_total*2} bytes, got {len(pixels)}')

        t0 = time.time()
        chunks = [pixels[i:i+CHUNK_BYTES] for i in range(0, npix_total*2, CHUNK_BYTES)]
        n = len(chunks)
        prev_npix = -1   # sentinel — track last written npix to skip redundant writes

        for idx, chunk in enumerate(chunks):
            # Pad last chunk to even word boundary
            if len(chunk) % 4:
                chunk += b'\x00' * (4 - len(chunk) % 4)
            npix = len(chunk) // 2

            # Write pixel data to mailbox
            self.ocd.write_block(MB_PIXELS, chunk)

            # Write window coords (only on first chunk)
            if idx == 0:
                # Pack window + npix in one block write (12 bytes at MB_X0)
                hdr = struct.pack('<HHHHHH', x0, y0, x1, y1, npix, 0)
                self.ocd.write_block(MB_X0, hdr)
                self.ocd.mww(MB_CMD, CMD_FRAME_START)
            else:
                # Only write NPIX if it changed (saves one mww for uniform chunks)
                if npix != prev_npix:
                    self.ocd.mww(MB_NPIX, npix)
                self.ocd.mww(MB_CMD, CMD_FRAME_CHUNK if idx < n-1 else CMD_FRAME_END)

            prev_npix = npix
            self._wait_idle()

        elapsed = time.time() - t0
        fps = 1.0 / elapsed if elapsed > 0 else 0
        if self.verbose:
            print(f'  frame {npix_total}px, {n} chunks, {elapsed*1000:.0f}ms, {fps:.1f}fps')
        return elapsed


# ════════════════════════════════════════════════════════════════════
# Tests
# ════════════════════════════════════════════════════════════════════

def test_colors(disp: Display):
    """Cycle through solid colors to verify the protocol."""
    colors = [
        ('BLACK',   COL_BLACK),
        ('WHITE',   COL_WHITE),
        ('RED',     COL_RED),
        ('GREEN',   COL_GREEN),
        ('BLUE',    COL_BLUE),
    ]
    print('Color test — 1s per color:')
    for name, col in colors:
        print(f'  {name}  (0x{col:04X})')
        disp.fill(col)
        time.sleep(1.0)
    print('Color test done.')


def test_gradient(disp: Display):
    """Fill screen with a full RGB565 gradient to verify blit_frame."""
    print('Gradient test...')
    buf = bytearray(LCD_W * LCD_H * 2)
    for y in range(LCD_H):
        for x in range(LCD_W):
            r = (x * 255) // (LCD_W - 1)
            g = (y * 255) // (LCD_H - 1)
            b = 128
            col = rgb565(r, g, b)
            i = (y * LCD_W + x) * 2
            # Store little-endian: MCU reads uint16 LE from SRAM
            buf[i]   = col & 0xFF   # low byte first
            buf[i+1] = col >> 8     # high byte second
    elapsed = disp.blit_frame(bytes(buf))
    print(f'  Gradient sent in {elapsed*1000:.0f}ms')
    time.sleep(2.0)


def test_checkerboard(disp: Display):
    """Black/white checkerboard pattern."""
    print('Checkerboard test...')
    buf = bytearray(LCD_W * LCD_H * 2)
    for y in range(LCD_H):
        for x in range(LCD_W):
            col = COL_WHITE if (x // 8 + y // 8) % 2 == 0 else COL_BLACK
            i = (y * LCD_W + x) * 2
            buf[i]   = col & 0xFF   # little-endian
            buf[i+1] = col >> 8
    elapsed = disp.blit_frame(bytes(buf))
    print(f'  Checkerboard sent in {elapsed*1000:.0f}ms')
    time.sleep(2.0)


def benchmark(disp: Display, n=10):
    """Measure raw frame throughput (alternating white/black frames)."""
    print(f'Benchmark: {n} white frames...')
    buf = bytes([0xFF, 0xFF] * (LCD_W * LCD_H))
    times = []
    for _ in range(n):
        t = disp.blit_frame(buf)
        times.append(t)
    avg = sum(times) / len(times)
    fps = 1.0 / avg
    print(f'  avg {avg*1000:.0f}ms/frame  =  {fps:.2f} fps')
    return fps


# ════════════════════════════════════════════════════════════════════
# Screen-capture streaming (uses PIL / OpenCV)
# ════════════════════════════════════════════════════════════════════

def stream_screen(disp: Display):
    """Continuously capture the full desktop, scale to 128×160, and stream it.

    Pixel byte-order note:
      write_block() passes bytes verbatim to SRAM.  The MCU reads each
      pixel as a little-endian uint16, so we must store pixels LE too.
      numpy .tobytes() on a '<u2' (LE uint16) array does exactly that —
      no slow Python loop needed.
    """
    try:
        import numpy as np
        from PIL import ImageGrab, Image
    except ImportError:
        print('ERROR: Pillow not installed. Run: pip install Pillow numpy')
        return

    print(f'Streaming full desktop -> {LCD_W}x{LCD_H} at ~2fps...')
    print('Press Ctrl-C to stop.')

    frame_n = 0
    t_start = time.time()
    try:
        while True:
            # Grab full primary screen (no region = whole screen)
            img = ImageGrab.grab()
            img = img.resize((LCD_W, LCD_H), Image.LANCZOS)
            arr = np.array(img, dtype='uint16')   # (H, W, 3), uint16

            # Convert RGB→BGR-swapped RGB565 for MADCTL=0x98
            # Formula: ((B>>3)<<11) | ((G>>2)<<5) | (R>>3)
            px = (((arr[:,:,2] >> 3) << 11) |
                  ((arr[:,:,1] >> 2) << 5)  |
                   (arr[:,:,0] >> 3)).astype('<u2')   # LE uint16

            # .tobytes() gives raw LE bytes — exactly what write_block needs
            buf = px.flatten().tobytes()

            disp.blit_frame(buf)
            frame_n += 1
            if frame_n % 5 == 0:
                fps = frame_n / (time.time() - t_start)
                print(f'  {frame_n} frames @ {fps:.1f} fps', flush=True)
    except KeyboardInterrupt:
        print(f'\nStopped after {frame_n} frames.')


# ════════════════════════════════════════════════════════════════════
# OpenOCD subprocess launcher
# ════════════════════════════════════════════════════════════════════

OPENOCD_CFG = r'/mnt/c/Users/cooli/Claude_Vapes/Vaporware/slotmachine/n32g031.openocd.cfg'

def start_openocd():
    """Launch OpenOCD in WSL background, wait until TCL port 6666 is ready.

    Runs:  wsl bash -c "openocd -f CFG ... & sleep 3600"
    The sleep keeps WSL alive (and OpenOCD with it).  The outer subprocess
    runs in background from Python so we don't block on the sleep.

    Usage:
        proc = start_openocd()
        ocd  = OpenOCD()          # connects on localhost:6666
        ...
        proc.terminate()
    """
    import os
    bash_cmd = (
        f"openocd"
        f" -f {OPENOCD_CFG}"
        f" -c 'n32g031.cpu configure -defer-examine'"
        f" -c 'init; n32g031.cpu arp_examine; halt; resume'"
        f" > /tmp/ocd.log 2>&1 &"
        f" sleep 3600"
    )
    env = dict(os.environ)
    env['MSYS_NO_PATHCONV'] = '1'
    proc = subprocess.Popen(
        ['wsl', 'bash', '-c', bash_cmd],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    # Poll until TCL port 6666 accepts connections
    import socket as _sock
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = _sock.socket()
            s.settimeout(1)
            s.connect(('localhost', 6666))
            s.close()
            return proc
        except OSError:
            time.sleep(0.5)
    raise TimeoutError('OpenOCD did not open port 6666 within 15 s')


# ════════════════════════════════════════════════════════════════════
# Entry point
# ════════════════════════════════════════════════════════════════════

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'test'

    print(f'doom_stream.py — mode={mode}')
    print('Connecting to OpenOCD TCL server on localhost:6666 ...')

    _ocd_proc = None
    try:
        ocd = OpenOCD('localhost', 6666)
    except ConnectionRefusedError:
        print('  OpenOCD not found — launching it automatically ...')
        _ocd_proc = start_openocd()
        ocd = OpenOCD('localhost', 6666)

    disp = Display(ocd, verbose=True)
    print('MCU ready.')

    try:
        if mode == 'test':
            test_colors(disp)
            test_checkerboard(disp)
            test_gradient(disp)
            benchmark(disp)

        elif mode == 'benchmark':
            benchmark(disp, n=20)

        elif mode == 'screen':
            stream_screen(disp)

        else:
            print(f'Unknown mode: {mode}')
            print('Usage: doom_stream.py [test|benchmark|screen]')
    finally:
        ocd.close()
        if _ocd_proc:
            _ocd_proc.terminate()


if __name__ == '__main__':
    main()
