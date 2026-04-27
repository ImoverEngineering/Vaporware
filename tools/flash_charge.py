"""Flash a .bin via existing OpenOCD telnet connection.

Usage:
    Edit BIN_PATH below to point at the example you want to flash, e.g.:
        examples/flappy/build/flappy.bin
        examples/slots/build/slots.bin
        examples/tamagotchi/build/tamagotchi.bin

    Requires an OpenOCD telnet server already running on PORT (default 6666).
    Start one with:  wsl openocd -f <example>/n32g031.openocd.cfg
"""
import socket, time

# Path to the .bin (WSL path — adjust example name and binary name as needed)
BIN_PATH = "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/flappy/build/flappy.bin"
FLASH_BASE = 0x08000000

HOST = "localhost"
PORT = 6666

import struct
with open(BIN_PATH, "rb") as f:
    data = f.read()
while len(data) % 4:
    data += b"\xff"
words = struct.unpack_from("<" + "I" * (len(data) // 4), data)
print("Binary: %d bytes = %d words" % (len(data), len(words)))

s = socket.socket()
s.settimeout(10.0)
s.connect((HOST, PORT))
time.sleep(0.1)

def cmd(s, c, timeout=8.0):
    s.sendall((c + '\x1a').encode())
    buf = b''
    t0 = time.time()
    while not buf.endswith(b'\x1a'):
        try:
            s.settimeout(2.0)
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            if time.time() - t0 > timeout:
                break
    return buf[:-1].decode(errors='replace').strip()

def mww(addr, val):
    cmd(s, 'mww 0x%08X 0x%08X' % (addr, val))

def mrw(addr):
    r = cmd(s, 'mrw 0x%08X' % addr)
    try:
        return int(r.strip(), 16)
    except:
        return None

def feed():
    mww(0x40003000, 0x0000AAAA)

print("Halting MCU...")
cmd(s, 'halt')
time.sleep(0.1)
cmd(s, 'halt')
time.sleep(0.05)

# Extend IWDG to ~26s
feed()
time.sleep(0.05)
mww(0x40003000, 0x00005555)  # unlock
time.sleep(0.02)
mww(0x40003004, 0x00000006)  # PR = /256
time.sleep(0.02)
mww(0x40003008, 0x00000FFF)  # RLR = 4095
time.sleep(0.02)
feed()
print("IWDG extended")

# Unlock flash
mww(0x40022004, 0x45670123)
mww(0x40022004, 0xCDEF89AB)
time.sleep(0.05)
mww(0x4002200C, 0x00000034)
time.sleep(0.02)

# Erase pages
npages = max(2, (len(data) + 511) // 512)
for i in range(npages):
    pg = FLASH_BASE + i * 0x200
    feed()
    print("Erasing 0x%08X..." % pg)
    mww(0x40022014, pg)
    mww(0x40022010, 0x00000002)
    mww(0x40022010, 0x00000042)
    time.sleep(0.05)
    # Poll BSY
    for _ in range(100):
        sr = mrw(0x4002200C)
        if sr is not None and not (sr & 1):
            break
        time.sleep(0.01)
    mww(0x40022010, 0x00000000)
    mww(0x4002200C, 0x00000034)

sr = mrw(0x4002200C)
print("After erase SR=0x%08X" % (sr or 0))

# Program
feed()
mww(0x40022010, 0x00000001)  # PG bit
feed()

for i, w in enumerate(words):
    if i % 30 == 0:
        feed()
    mww(FLASH_BASE + i * 4, w)

time.sleep(0.05)
mww(0x40022010, 0x00000000)
sr = mrw(0x4002200C)
print("Post-write SR=0x%08X" % (sr or 0))

f0 = mrw(FLASH_BASE)
print("f0=0x%08X  expect=0x%08X  match=%s" % (f0 or 0, words[0], (f0 == words[0])))

feed()
print("Booting...")
cmd(s, 'reset run')
s.close()
print("Done.")
