#!/usr/bin/env python3
"""
Diagnostic script to figure out why GC9107 shows nothing.
1. Send INVON (0x21) directly via raw SPI to see if display inverts (proves alive)
2. Try blit path (FRAME_START/CHUNK/END) vs fill path
3. Force backlight via GPIO writes
"""
import socket, struct, time
import cv2
import numpy as np

class OpenOCD:
    def __init__(self):
        self.s = socket.socket()
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(10.0)
        self.s.connect(('localhost', 6666))
    def cmd(self, tcl, timeout=10.0):
        self.s.sendall((tcl + '\x1a').encode())
        buf = b''
        self.s.settimeout(timeout)
        while not buf.endswith(b'\x1a'):
            chunk = self.s.recv(4096)
            if not chunk: break
            buf += chunk
        return buf[:-1].decode(errors='replace').strip()
    def mdw(self, addr):
        resp = self.cmd(f'mdw 0x{addr:08X}')
        try: return int(resp.split(':')[1].strip().split()[0], 16)
        except: return None
    def mww(self, addr, val): return self.cmd(f'mww 0x{addr:08X} 0x{val:08X}')
    def mwh(self, addr, val): return self.cmd(f'mwh 0x{addr:04X} 0x{val:04X}')
    def write_block(self, addr, data: bytes):
        if len(data) % 4: data += b'\x00' * (4 - len(data) % 4)
        words = struct.unpack(f'<{len(data)//4}I', data)
        return self.cmd(f'write_memory 0x{addr:08X} 32 {{ {" ".join(f"0x{w:08X}" for w in words)} }}', timeout=30.0)
    def close(self): self.s.close()

MB = 0x20001000
CMD = MB; PIX = MB+0x10; NPIX = MB+0xC
CMD_IDLE=0; CMD_FILL=1; CMD_FRAME_START=2; CMD_FRAME_CHUNK=3; CMD_FRAME_END=4
BASE = r'C:\Users\cooli\Claude_Vapes'

# GPIO/SPI registers (N32G031, STM32G0-style)
GPIOA = 0x40010800
GPIOB = 0x40010C00
GPIOA_BSRR = GPIOA + 0x18
GPIOB_BSRR = GPIOB + 0x18
GPIOB_IDR  = GPIOB + 0x10
GPIOA_IDR  = GPIOA + 0x10
SPI1_DR    = 0x4001300C
SPI1_SR    = 0x40013008
SPI1_CR1   = 0x40013000

# Pin defs
CS_PIN   = 15  # PA15
DC_PIN   =  7  # PB7
BL_PIN   =  4  # PB4
RST_PIN  =  6  # PB6

cap = cv2.VideoCapture(1, cv2.CAP_DSHOW)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
for _ in range(20): cap.read()

def snap():
    for _ in range(5): cap.read()
    ret, f = cap.read()
    return f if ret else None

def disp_crop(frame):
    """Crop to display panel area."""
    if frame is None: return None
    return frame[230:510, 440:760]

ocd = OpenOCD()

def fill_via_mailbox(color):
    ocd.write_block(PIX, struct.pack('<HH', color, color))
    ocd.mww(NPIX, 1)
    ocd.mww(CMD, CMD_FILL)
    t0 = time.time()
    while True:
        v = ocd.mdw(CMD)
        if v == CMD_IDLE: return
        if time.time()-t0 > 8: print("  fill timeout!"); return
        time.sleep(0.02)

# ─── Test 1: Check backlight by toggling PB4 ───────────────────────────
print("=== Test 1: Backlight toggle ===")
print("Current GPIOB_IDR:", hex(ocd.mdw(GPIOB_IDR) or 0))
print("  PB4 state:", (ocd.mdw(GPIOB_IDR) or 0) >> 4 & 1, "(0=ON, 1=OFF)")

# Fill white first
fill_via_mailbox(0xFFFF)
time.sleep(0.3)
f_bl_on = snap()
if f_bl_on is not None:
    cv2.imwrite(f'{BASE}/diag_bl_on.jpg', f_bl_on)
    crop_on = disp_crop(f_bl_on)
    print(f"  BL=LOW(ON): display brightness={cv2.cvtColor(crop_on, cv2.COLOR_BGR2GRAY).mean():.1f}")

# Force BL HIGH (off) - set PB4 HIGH via BSRR
ocd.mww(GPIOB_BSRR, 1 << BL_PIN)  # BS4 = set PB4 HIGH = backlight OFF
time.sleep(0.3)
f_bl_off = snap()
if f_bl_off is not None:
    cv2.imwrite(f'{BASE}/diag_bl_off.jpg', f_bl_off)
    crop_off = disp_crop(f_bl_off)
    print(f"  BL=HIGH(OFF): display brightness={cv2.cvtColor(crop_off, cv2.COLOR_BGR2GRAY).mean():.1f}")

# Force BL LOW (on) again
ocd.mww(GPIOB_BSRR, 1 << (BL_PIN + 16))  # BR4 = set PB4 LOW = backlight ON
time.sleep(0.3)
f_bl_on2 = snap()
if f_bl_on2 is not None:
    cv2.imwrite(f'{BASE}/diag_bl_on2.jpg', f_bl_on2)
    crop_on2 = disp_crop(f_bl_on2)
    print(f"  BL=LOW(ON) again: display brightness={cv2.cvtColor(crop_on2, cv2.COLOR_BGR2GRAY).mean():.1f}")

# ─── Test 2: Blit half-white half-black via FRAME protocol ────────────
print("\n=== Test 2: Half-white/half-black blit ===")
LCD_W, LCD_H = 128, 160
CHUNK_PIX = 128  # use smaller chunks

# Build frame: top half white, bottom half black
frame_buf = bytearray(LCD_W * LCD_H * 2)
for y in range(LCD_H):
    for x in range(LCD_W):
        color = 0xFFFF if y < LCD_H//2 else 0x0000
        i = (y * LCD_W + x) * 2
        frame_buf[i]   = color & 0xFF   # LE
        frame_buf[i+1] = color >> 8

total_pix = LCD_W * LCD_H
chunk_bytes = CHUNK_PIX * 2
chunks = [bytes(frame_buf[i:i+chunk_bytes]) for i in range(0, total_pix*2, chunk_bytes)]
n = len(chunks)
print(f"  Sending {n} chunks of {CHUNK_PIX} pixels...")

for idx, chunk in enumerate(chunks):
    if len(chunk) % 4: chunk += b'\x00' * (4 - len(chunk) % 4)
    npix = len(chunk) // 2
    ocd.write_block(PIX, chunk)
    if idx == 0:
        hdr = struct.pack('<HHHHHH', 0, 0, LCD_W-1, LCD_H-1, npix, 0)
        ocd.write_block(MB + 4, hdr)
        ocd.mww(CMD, CMD_FRAME_START)
    else:
        ocd.mww(NPIX, npix)
        ocd.mww(CMD, CMD_FRAME_CHUNK if idx < n-1 else CMD_FRAME_END)
    t0 = time.time()
    while True:
        v = ocd.mdw(CMD)
        if v == CMD_IDLE: break
        if time.time()-t0 > 5: print(f"    chunk {idx} timeout!"); break
        time.sleep(0.01)

time.sleep(0.5)
f_split = snap()
if f_split is not None:
    cv2.imwrite(f'{BASE}/diag_split.jpg', f_split)
    crop_split = disp_crop(f_split)
    cv2.imwrite(f'{BASE}/diag_split_crop.jpg', cv2.resize(crop_split, None, fx=2, fy=2))
    g = cv2.cvtColor(crop_split, cv2.COLOR_BGR2GRAY)
    # Check top vs bottom of crop
    mid = g.shape[0]//2
    print(f"  Top half brightness: {g[:mid].mean():.1f}")
    print(f"  Bottom half brightness: {g[mid:].mean():.1f}")
    print(f"  (if display works: top should be ~brighter for white, bottom ~darker for black)")

# ─── Test 3: INVON command to see if display responds ──────────────────
print("\n=== Test 3: INVON command ===")
# Wait for MCU to be idle, then check if we can observe any change
# by toggling INVON with white fill
fill_via_mailbox(0xFFFF)
time.sleep(0.5)
f_before = snap()

# We can't easily send raw SPI commands while the MCU is running the mailbox loop.
# Instead: check if toggling BL causes any visible difference to confirm which
# is the backlight enable direction.

cap.release()
ocd.close()

# Compare bl_on vs bl_off
if f_bl_on is not None and f_bl_off is not None:
    g_on = cv2.cvtColor(disp_crop(f_bl_on), cv2.COLOR_BGR2GRAY).astype(float)
    g_off = cv2.cvtColor(disp_crop(f_bl_off), cv2.COLOR_BGR2GRAY).astype(float)
    diff = g_on.mean() - g_off.mean()
    print(f"\nBL toggle diff: on={g_on.mean():.1f}  off={g_off.mean():.1f}  delta={diff:.2f}")
    if abs(diff) > 5:
        print("  -> Backlight IS responding!")
    else:
        print("  -> Backlight shows NO change (backlight pin may not be connected)")

print("\nDone.")
