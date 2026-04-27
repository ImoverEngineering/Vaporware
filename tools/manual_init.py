"""manual_init.py - Manually reinit GC9107 via OpenOCD while MCU halted."""
import socket, time

s = socket.socket()
s.settimeout(10.0)
s.connect(('localhost', 6666))

def cmd(c, timeout=5.0):
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
            if time.time()-t0>timeout: break
    return buf[:-1].decode(errors='replace').strip()

def mdw(addr):
    r = cmd('mdw 0x%08X' % addr)
    try: return int(r.split(':')[1].strip().split()[0], 16)
    except: return None

def mww(addr, val): cmd('mww 0x%08X 0x%08X' % (addr, val))

# GPIO/SPI addresses (N32G031)
GPIOA_BSRR = 0x40010818
GPIOB_BSRR = 0x40010C18
SPI1_CR1   = 0x40012000
SPI1_SR    = 0x40012008
SPI1_DR    = 0x4001200C

CS_HIGH  = 1 << 15
CS_LOW   = 1 << (15+16)
DC_HIGH  = 1 << 7
DC_LOW   = 1 << (7+16)
RST_HIGH = 1 << 6
RST_LOW  = 1 << (6+16)
BL_HIGH  = 1 << 4
BL_LOW   = 1 << (4+16)

cmd('halt')
time.sleep(0.05)

cr1 = mdw(SPI1_CR1)
print('SPI1 CR1=0x%04X (SPE=%d)' % (cr1 or 0, (cr1>>6)&1 if cr1 else 0))

def spi_write(byte):
    mww(SPI1_DR, byte & 0xFF)

def lcd_cmd(c):
    mww(GPIOB_BSRR, DC_LOW)
    spi_write(c)

def lcd_data(d):
    mww(GPIOB_BSRR, DC_HIGH)
    spi_write(d)

print('RST pulse: LOW 150ms -> HIGH 150ms')
mww(GPIOA_BSRR, CS_HIGH)   # CS high while resetting
mww(GPIOB_BSRR, RST_LOW)
time.sleep(0.15)
mww(GPIOB_BSRR, RST_HIGH)
time.sleep(0.15)

print('Assert CS, Sleep Out + 130ms')
mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x11)
time.sleep(0.13)

print('Init sequence...')
lcd_cmd(0xFF); lcd_data(0xA5)
lcd_cmd(0x3E); lcd_data(0x08)
lcd_cmd(0x3A); lcd_data(0x65)
lcd_cmd(0x82); lcd_data(0x00)
lcd_cmd(0x98); lcd_data(0x00)
lcd_cmd(0x63); lcd_data(0x0F)
lcd_cmd(0x64); lcd_data(0x0F)
lcd_cmd(0xB4); lcd_data(0x34)
lcd_cmd(0xB5); lcd_data(0x30)
lcd_cmd(0x83); lcd_data(0x13)
lcd_cmd(0x86); lcd_data(0x04)
lcd_cmd(0x87); lcd_data(0x19)
lcd_cmd(0x88); lcd_data(0x2F)
lcd_cmd(0x89); lcd_data(0x36)
lcd_cmd(0x93); lcd_data(0x63)
lcd_cmd(0x96); lcd_data(0x81)
lcd_cmd(0xC3); lcd_data(0x10)
lcd_cmd(0xE6); lcd_data(0x00)
lcd_cmd(0x99); lcd_data(0x01)
lcd_cmd(0x44); lcd_data(0x00)
gamma_p = [(0x70,0x07),(0x71,0x19),(0x72,0x1A),(0x73,0x13),(0x74,0x19),
           (0x75,0x1D),(0x76,0x47),(0x77,0x0A),(0x78,0x07),(0x79,0x47),
           (0x7A,0x05),(0x7B,0x09),(0x7C,0x0D),(0x7D,0x0C),(0x7E,0x0C),(0x7F,0x08)]
for c,d in gamma_p: lcd_cmd(c); lcd_data(d)
gamma_n = [(0xA0,0x0B),(0xA1,0x36),(0xA2,0x09),(0xA3,0x0D),(0xA4,0x08),
           (0xA5,0x23),(0xA6,0x3B),(0xA7,0x04),(0xA8,0x07),(0xA9,0x38),
           (0xAA,0x0A),(0xAB,0x12),(0xAC,0x0C),(0xAD,0x07),(0xAE,0x2F),(0xAF,0x07)]
for c,d in gamma_n: lcd_cmd(c); lcd_data(d)
lcd_cmd(0xFF); lcd_data(0x00)

lcd_cmd(0x36); lcd_data(0x98)  # MADCTL: MY=1, ML=1, BGR=1
lcd_cmd(0x29)                   # Display ON
time.sleep(0.015)

print('Fill WHITE (128x160 = 20480 pixels)...')
# CASET: 0..127
lcd_cmd(0x2A)
lcd_data(0x00); lcd_data(0x00)
lcd_data(0x00); lcd_data(0x7F)
# RASET: 0..159
lcd_cmd(0x2B)
lcd_data(0x00); lcd_data(0x00)
lcd_data(0x00); lcd_data(0x9F)
# RAMWR
lcd_cmd(0x2C)
mww(GPIOB_BSRR, DC_HIGH)

for i in range(128*160):
    spi_write(0xFF)
    spi_write(0xFF)

mww(GPIOA_BSRR, CS_HIGH)
mww(GPIOB_BSRR, BL_LOW)   # backlight ON

print('Holding halted 3s - look at display!')
time.sleep(3.0)

# Also try RED fill to distinguish from white background
print('Fill RED (0x001F with BGR=1 = red on screen)...')
mww(GPIOA_BSRR, CS_LOW)
lcd_cmd(0x2A)
lcd_data(0x00); lcd_data(0x00)
lcd_data(0x00); lcd_data(0x7F)
lcd_cmd(0x2B)
lcd_data(0x00); lcd_data(0x00)
lcd_data(0x00); lcd_data(0x9F)
lcd_cmd(0x2C)
mww(GPIOB_BSRR, DC_HIGH)
for i in range(128*160):
    spi_write(0x00)
    spi_write(0x1F)
mww(GPIOA_BSRR, CS_HIGH)

print('RED fill done - holding 3s...')
time.sleep(3.0)

print('Resuming MCU...')
cmd('resume')
s.close()
print('Done.')
