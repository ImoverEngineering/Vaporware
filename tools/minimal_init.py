"""minimal_init.py
Try the SIMPLEST possible GC9107 init to rule out sequence issues.
No manufacturer-register unlocking, just: RST + SleepOut + COLMOD + DisplayOn + fill.
Try multiple COLMOD values.
"""
import socket, time

s = socket.socket()
s.settimeout(10.0)
s.connect(('localhost', 6666))

def cmd(c):
    s.sendall((c+'\x1a').encode())
    buf = b''
    t0=time.time()
    while not buf.endswith(b'\x1a'):
        try:
            s.settimeout(2.0)
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
        except:
            if time.time()-t0>4: break
    return buf[:-1].decode(errors='replace').strip()

def mww(addr, val): cmd('mww 0x%08X 0x%08X' % (addr, val))
def mdw(addr):
    r = cmd('mdw 0x%08X' % addr)
    try: return int(r.split(':')[1].strip().split()[0], 16)
    except: return None

GPIOA_BSRR = 0x40010818
GPIOB_BSRR = 0x40010C18
SPI1_DR    = 0x4001200C

CS_HIGH  = 1 << 15;   CS_LOW   = 1 << (15+16)
DC_HIGH  = 1 << 7;    DC_LOW   = 1 << (7+16)
RST_HIGH = 1 << 6;    RST_LOW  = 1 << (6+16)
BL_HIGH  = 1 << 4;    BL_LOW   = 1 << (4+16)

cmd('halt')
time.sleep(0.1)

def spi_byte(b):    mww(SPI1_DR, b & 0xFF)
def lcd_cmd(c):     mww(GPIOB_BSRR, DC_LOW);  spi_byte(c)
def lcd_data(d):    mww(GPIOB_BSRR, DC_HIGH); spi_byte(d)

def do_rst():
    print('  RST pulse...')
    mww(GPIOA_BSRR, CS_HIGH)
    mww(GPIOB_BSRR, RST_LOW)
    time.sleep(0.20)
    mww(GPIOB_BSRR, RST_HIGH)
    time.sleep(0.20)

def fill_solid(hi, lo, label):
    """Fill 128x160 with color (hi byte, lo byte)."""
    mww(GPIOA_BSRR, CS_LOW)
    lcd_cmd(0x2A)                              # CASET
    lcd_data(0x00); lcd_data(0x00)
    lcd_data(0x00); lcd_data(0x7F)             # end col 127
    lcd_cmd(0x2B)                              # RASET
    lcd_data(0x00); lcd_data(0x00)
    lcd_data(0x00); lcd_data(0x9F)             # end row 159
    lcd_cmd(0x2C)                              # RAMWR
    mww(GPIOB_BSRR, DC_HIGH)
    print(f'  Filling {label} (128x160)...')
    for _ in range(128*160):
        spi_byte(hi)
        spi_byte(lo)
    mww(GPIOA_BSRR, CS_HIGH)
    print(f'  {label} fill done. Holding 4s - look at screen!')
    mww(GPIOB_BSRR, BL_LOW)   # ensure backlight on
    time.sleep(4.0)

# ── Attempt 1: Minimal with COLMOD=0x55 (standard 65K) ──
print('\n=== Attempt 1: Minimal init, COLMOD=0x55 ===')
do_rst()
mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x11)                  # Sleep Out
time.sleep(0.13)
lcd_cmd(0x3A); lcd_data(0x55) # COLMOD 16-bit (0x55 = standard)
lcd_cmd(0x36); lcd_data(0x00) # MADCTL default (no flip, no BGR)
lcd_cmd(0x29)                  # Display ON
time.sleep(0.015)
mww(GPIOA_BSRR, CS_HIGH)
fill_solid(0xFF, 0xFF, 'WHITE(COLMOD=0x55)')

# ── Attempt 2: Minimal with COLMOD=0x65 + MADCTL BGR ──
print('\n=== Attempt 2: Minimal init, COLMOD=0x65, MADCTL=0x98 ===')
do_rst()
mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x11)
time.sleep(0.13)
lcd_cmd(0x3A); lcd_data(0x65)  # COLMOD as original firmware
lcd_cmd(0x36); lcd_data(0x98)  # MADCTL: MY=1, ML=1, BGR=1
lcd_cmd(0x29)
time.sleep(0.015)
mww(GPIOA_BSRR, CS_HIGH)
fill_solid(0xFF, 0xFF, 'WHITE(COLMOD=0x65)')

# ── Attempt 3: Inversion ON (0x21) to test if display responds at all ──
print('\n=== Attempt 3: Invert ON command after Attempt 2 ===')
mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x21)                  # Display Inversion ON (should make screen show inverted)
mww(GPIOA_BSRR, CS_HIGH)
mww(GPIOB_BSRR, BL_LOW)
print('  INVON sent - screen should look different (inverted) if responding')
time.sleep(4.0)

mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x20)                  # Inversion OFF
mww(GPIOA_BSRR, CS_HIGH)
time.sleep(2.0)

print('\nResuming MCU...')
cmd('resume')
s.close()
print('Done.')
