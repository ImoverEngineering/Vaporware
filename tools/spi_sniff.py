#!/usr/bin/env python3
"""
spi_sniff.py - Monitor GPIO/SPI activity during CMD_FILL to verify
that CS toggles and SPI actually transmits.
"""
import socket, struct, time

class OpenOCD:
    def __init__(self, host='localhost', port=6666, timeout=10.0):
        self.s = socket.socket()
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(timeout)
        self.s.connect((host, port))
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
    def mww(self, addr, val):
        return self.cmd(f'mww 0x{addr:08X} 0x{val:08X}')
    def write_block(self, addr, data: bytes):
        if len(data) % 4: data += b'\x00' * (4 - len(data) % 4)
        words = struct.unpack(f'<{len(data)//4}I', data)
        return self.cmd(f'write_memory 0x{addr:08X} 32 {{ {" ".join(f"0x{w:08X}" for w in words)} }}', timeout=30.0)
    def close(self): self.s.close()

# ── Addresses ──────────────────────────────────────────────────────────
GPIOA     = 0x40010800
GPIOB     = 0x40010C00
SPI1      = 0x40012000
O_IDR     = 0x10
O_ODR     = 0x14

SPI1_CR1  = SPI1 + 0x00
SPI1_SR   = SPI1 + 0x08

MB_BASE   = 0x20001000
MB_CMD    = MB_BASE
MB_NPIX   = MB_BASE + 0x00C
MB_PIXELS = MB_BASE + 0x010
CMD_IDLE  = 0
CMD_FILL  = 1

ocd = OpenOCD()

print("=== Pre-fill state ===")
print(f"GPIOA_IDR = 0x{ocd.mdw(GPIOA+O_IDR):08X}  PA15(CS)={(ocd.mdw(GPIOA+O_IDR)>>15)&1}")
print(f"GPIOB_IDR = 0x{ocd.mdw(GPIOB+O_IDR):08X}")
print(f"SPI1_CR1  = 0x{ocd.mdw(SPI1_CR1):08X}")
spi_sr = ocd.mdw(SPI1_SR)
print(f"SPI1_SR   = 0x{spi_sr:08X}  TXE={(spi_sr>>1)&1} BSY={(spi_sr>>7)&1} OVR={(spi_sr>>6)&1}")
print(f"MB_CMD    = 0x{ocd.mdw(MB_CMD):08X}")
print()

# ── Send CMD_FILL white and monitor CS during execution ────────────────
print("=== Sending CMD_FILL 0xFFFF and monitoring CS ===")
# Write mailbox
ocd.write_block(MB_PIXELS, struct.pack('<HH', 0xFFFF, 0xFFFF))
ocd.mww(MB_NPIX, 1)

# Start fill and immediately poll CS
t_start = time.time()
ocd.mww(MB_CMD, CMD_FILL)

cs_history = []
sr_history = []
for _ in range(20):
    pa_idr = ocd.mdw(GPIOA + O_IDR)
    pb_idr = ocd.mdw(GPIOB + O_IDR)
    spi_sr = ocd.mdw(SPI1_SR)
    mb_cmd = ocd.mdw(MB_CMD)
    t = time.time() - t_start
    cs = (pa_idr >> 15) & 1
    bsy = (spi_sr >> 7) & 1
    txe = (spi_sr >> 1) & 1
    ovr = (spi_sr >> 6) & 1
    dc = (pb_idr >> 7) & 1
    rst = (pb_idr >> 6) & 1
    mosi = (pb_idr >> 5) & 1
    sck = (pb_idr >> 3) & 1
    cs_history.append(cs)
    sr_history.append((bsy, txe))
    print(f"  t={t*1000:5.1f}ms  CS={cs}  BSY={bsy} TXE={txe} OVR={ovr}  "
          f"SCK={sck} MOSI={mosi} DC={dc} RST={rst}  MB_CMD=0x{mb_cmd:X}")
    if mb_cmd == CMD_IDLE and t > 0.05:
        break

print()
print(f"CS went LOW (asserted): {'YES' if 0 in cs_history else 'NO - CS never went LOW!'}")
print(f"SPI BSY was HIGH: {'YES' if any(b for b,_ in sr_history) else 'NO - SPI never busy!'}")
print()

# ── Summary ─────────────────────────────────────────────────────────────
print("=== Post-fill state ===")
pa_idr = ocd.mdw(GPIOA + O_IDR)
pb_idr = ocd.mdw(GPIOB + O_IDR)
spi_sr = ocd.mdw(SPI1_SR)
print(f"GPIOA_IDR = 0x{pa_idr:08X}  PA15(CS)={(pa_idr>>15)&1}")
print(f"GPIOB_IDR = 0x{pb_idr:08X}  SCK={(pb_idr>>3)&1} MOSI={(pb_idr>>5)&1} DC={(pb_idr>>7)&1}")
print(f"SPI1_SR   = 0x{spi_sr:08X}  TXE={(spi_sr>>1)&1} BSY={(spi_sr>>7)&1} OVR={(spi_sr>>6)&1}")
print(f"MB_CMD    = 0x{ocd.mdw(MB_CMD):08X}  {'IDLE' if ocd.mdw(MB_CMD)==0 else 'BUSY'}")

# ── Check actual GPIOA_ODR to confirm PA15 output state ─────────────────
print()
print("=== PA15 ODR check ===")
pa_odr = ocd.mdw(GPIOA + O_ODR)
print(f"GPIOA_ODR = 0x{pa_odr:08X}  PA15(CS)={(pa_odr>>15)&1}  (1=deasserted/HIGH)")

# Wait for fill to complete and read one more time
time.sleep(0.2)
v = ocd.mdw(MB_CMD)
print(f"\nFinal MB_CMD = 0x{v:08X}  {'IDLE' if v==0 else 'still busy'}")

ocd.close()
