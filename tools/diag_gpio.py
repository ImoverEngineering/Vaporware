#!/usr/bin/env python3
"""
diag_gpio.py — Read live GPIO/SPI register state from the MCU via OpenOCD.
N32G031 uses STM32G0-style GPIO: MODER/OTYPER/OSPEEDR/PUPDR/IDR/ODR/BSRR/AFRL/AFRH
"""
import socket, struct, sys

class OpenOCD:
    def __init__(self, host='localhost', port=6666, timeout=5.0):
        self.s = socket.socket()
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(timeout)
        self.s.connect((host, port))

    def cmd(self, tcl):
        self.s.sendall((tcl + '\x1a').encode())
        buf = b''
        self.s.settimeout(5.0)
        while not buf.endswith(b'\x1a'):
            buf += self.s.recv(4096)
        return buf[:-1].decode(errors='replace').strip()

    def mdw(self, addr):
        resp = self.cmd(f'mdw 0x{addr:08X}')
        try:
            return int(resp.split(':')[1].strip().split()[0], 16)
        except Exception:
            return None

    def mww(self, addr, val):
        return self.cmd(f'mww 0x{addr:08X} 0x{val:08X}')

    def close(self):
        self.s.close()

ocd = OpenOCD()

# ── GPIO base addresses ──────────────────────────────────────────────
GPIOA = 0x40010800
GPIOB = 0x40010C00
# Offsets (STM32G0/N32G031 style)
O_MODER   = 0x00
O_OTYPER  = 0x04
O_OSPEEDR = 0x08
O_PUPDR   = 0x0C
O_IDR     = 0x10
O_ODR     = 0x14
O_BSRR    = 0x18
O_AFRL    = 0x20   # pins 0-7
O_AFRH    = 0x24   # pins 8-15

SPI1_BASE = 0x40012000
SPI1_CR1  = SPI1_BASE + 0x00
SPI1_CR2  = SPI1_BASE + 0x04
SPI1_SR   = SPI1_BASE + 0x08

def read_reg(base, off, name):
    v = ocd.mdw(base + off)
    print(f"  {name:30s} = 0x{v:08X}" if v is not None else f"  {name:30s} = ERROR")
    return v

def decode_moder(v, name):
    modes = ['input', 'output', 'AF', 'analog']
    pins = []
    for pin in range(16):
        m = (v >> (pin*2)) & 3
        if m != 3:  # 3=analog = default/unused
            pins.append(f"P{name[4]}{pin}={modes[m]}")
    print(f"    Active pins: {', '.join(pins) if pins else 'all analog'}")

def decode_afrl(v, port_letter):
    print(f"    AFRL: ", end='')
    for pin in range(8):
        af = (v >> (pin*4)) & 0xF
        if af != 0:
            print(f"P{port_letter}{pin}=AF{af} ", end='')
    print()

def decode_afrh(v, port_letter):
    print(f"    AFRH: ", end='')
    for pin in range(8):
        af = (v >> (pin*4)) & 0xF
        if af != 0:
            print(f"P{port_letter}{pin+8}=AF{af} ", end='')
    print()

print("=" * 60)
print("GPIOA registers (CS=PA15, SWD=PA13/PA14)")
print("=" * 60)
ma = read_reg(GPIOA, O_MODER,   'GPIOA_MODER')
ot_a = read_reg(GPIOA, O_OTYPER,  'GPIOA_OTYPER')
read_reg(GPIOA, O_PUPDR,   'GPIOA_PUPDR')
ia = read_reg(GPIOA, O_IDR,     'GPIOA_IDR')
oa = read_reg(GPIOA, O_ODR,     'GPIOA_ODR')
aflh_a = read_reg(GPIOA, O_AFRH,    'GPIOA_AFRH')
if ma is not None:
    decode_moder(ma, 'GPIOA')
    print(f"    PA15(CS) MODER={( ma>>30)&3}  PA14(SWDCK)={(ma>>28)&3}  PA13(SWDIO)={(ma>>26)&3}")
    print(f"    PA15 IDR={(ia>>15)&1}  ODR={(oa>>15)&1}  OTYPER={(ot_a>>15)&1}")
if aflh_a is not None:
    decode_afrh(aflh_a, 'A')
    print(f"    PA15 AF={(aflh_a>>28)&0xF}")

print()
print("=" * 60)
print("GPIOB registers (SCK=PB3, MOSI=PB5, BL=PB4, RST=PB6, DC=PB7)")
print("=" * 60)
mb = read_reg(GPIOB, O_MODER,   'GPIOB_MODER')
ot_b = read_reg(GPIOB, O_OTYPER,  'GPIOB_OTYPER')
read_reg(GPIOB, O_PUPDR,   'GPIOB_PUPDR')
ib = read_reg(GPIOB, O_IDR,     'GPIOB_IDR')
ob = read_reg(GPIOB, O_ODR,     'GPIOB_ODR')
afl_b = read_reg(GPIOB, O_AFRL,    'GPIOB_AFRL')
if mb is not None:
    decode_moder(mb, 'GPIOB')
    print(f"    PB3(SCK)  MODER={(mb>>6)&3}  OTYPER={(ot_b>>3)&1}  IDR={(ib>>3)&1}  ODR={(ob>>3)&1}  AF={(afl_b>>12)&0xF}")
    print(f"    PB4(BL)   MODER={(mb>>8)&3}  OTYPER={(ot_b>>4)&1}  IDR={(ib>>4)&1}  ODR={(ob>>4)&1}")
    print(f"    PB5(MOSI) MODER={(mb>>10)&3}  OTYPER={(ot_b>>5)&1}  IDR={(ib>>5)&1}  ODR={(ob>>5)&1}  AF={(afl_b>>20)&0xF}")
    print(f"    PB6(RST)  MODER={(mb>>12)&3}  OTYPER={(ot_b>>6)&1}  IDR={(ib>>6)&1}  ODR={(ob>>6)&1}")
    print(f"    PB7(DC)   MODER={(mb>>14)&3}  OTYPER={(ot_b>>7)&1}  IDR={(ib>>7)&1}  ODR={(ob>>7)&1}")
if afl_b is not None:
    decode_afrl(afl_b, 'B')

print()
print("=" * 60)
print("SPI1 registers")
print("=" * 60)
cr1 = read_reg(SPI1_BASE, 0x00, 'SPI1_CR1')
cr2 = read_reg(SPI1_BASE, 0x04, 'SPI1_CR2')
sr  = read_reg(SPI1_BASE, 0x08, 'SPI1_SR')
if cr1 is not None:
    spe = (cr1 >> 6) & 1
    mstr = (cr1 >> 2) & 1
    br = (cr1 >> 3) & 7
    cpol = (cr1 >> 1) & 1
    cpha = (cr1 >> 0) & 1
    ssm = (cr1 >> 9) & 1
    ssi = (cr1 >> 8) & 1
    print(f"    SPE={spe} MSTR={mstr} BR={br}(div={2**(br+1)}) CPOL={cpol} CPHA={cpha} SSM={ssm} SSI={ssi}")
    print(f"    Mode: {'SPI Master' if mstr else 'SPI Slave'}, {'enabled' if spe else 'DISABLED!'}")

print()
print("=" * 60)
print("Mailbox cmd value")
print("=" * 60)
cmd = ocd.mdw(0x20001000)
print(f"  MB_CMD = 0x{cmd:08X}  ({'IDLE' if cmd==0 else 'BUSY/ERROR'})")

ocd.close()
