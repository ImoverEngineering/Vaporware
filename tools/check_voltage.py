#!/usr/bin/env python3
"""
check_voltage.py — Read vape battery voltage via SWD/OpenOCD.

Reads battery ADC (PB0 / channel 8 / ADC base 0x40020800) via OpenOCD.
Calibration from config.h:  Vbat = raw * 28 * 3.0 / 4096
  Raw 205 = 4.20V (full)   Raw 181 = 3.70V   Raw 146 = 3.00V (warn)

Usage:
  python check_voltage.py              # single reading
  python check_voltage.py --watch 30  # poll every 30 seconds

Requires OpenOCD running in WSL (or starts it automatically).
"""
import socket, time, subprocess, sys, argparse

# ── Config ───────────────────────────────────────────────────────────────────
OPENOCD_CFG = "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/tamagotchi/n32g031.openocd.cfg"
OPENOCD_LOG = "/tmp/ocd_voltage.log"
OCD_HOST    = "localhost"
OCD_PORT    = 6666

# ADC / GPIO register addresses (N32G031, Raz DC25000 board)
AHBENR      = 0x40021014   # RCC AHB clock enable  (bit 12 = ADC)
RCC_CFGR2   = 0x4002102C   # ADC prescaler config
GPIOB_MODER = 0x40010C00
GPIOB_IDR   = 0x40010C10
ADC_BASE    = 0x40020800
ADC_STS     = ADC_BASE + 0x00
ADC_CTRL2   = ADC_BASE + 0x08
ADC_SMPR2   = ADC_BASE + 0x10
ADC_RSEQ1   = ADC_BASE + 0x30
ADC_RSEQ3   = ADC_BASE + 0x38
ADC_DAT     = ADC_BASE + 0x50

# Battery calibration
BAT_CHANNEL = 8
BAT_GPIO    = 0          # PB0
# NOTE: config.h says DIVIDER=28 (28:1 voltage divider) but that was never
# correct for this board.  Empirically the divider ratio is ~0.71 (Vadc/Vbat),
# so the multiplier is 1/0.71 ≈ 1.41.  Readings via the firmware mailbox
# (charge firmware with 100 ms settling delay) confirm raw~3920 at Vbat~4V.
DIVIDER     = 1.41
VDDA        = 3.0
ADC_MAX     = 4096.0
BAT_FULL_V  = 4.20
BAT_WARN_V  = 3.00
BAT_CRIT_V  = 2.50

# RAM mailbox written by charge firmware
MAILBOX_RAW   = 0x20000000
MAILBOX_COUNT = 0x20000004

# ── OpenOCD connection ────────────────────────────────────────────────────────
def ocd_connect(retries=3):
    for i in range(retries):
        try:
            s = socket.socket()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(10.0)
            s.connect((OCD_HOST, OCD_PORT))
            return s
        except ConnectionRefusedError:
            if i == 0:
                print("OpenOCD not running — starting in WSL...")
                _start_openocd()
            time.sleep(3)
    raise RuntimeError("Could not connect to OpenOCD after retries")

def _start_openocd():
    subprocess.Popen(
        ["wsl", "bash", "-c",
         f"pkill -9 openocd 2>/dev/null; sleep 1; "
         f"openocd -f {OPENOCD_CFG} > {OPENOCD_LOG} 2>&1"],
        creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
    )
    time.sleep(5)

def ocd_cmd(s, tcl, timeout=10.0):
    s.sendall((tcl + '\x1a').encode())
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

def mdw(s, addr):
    r = ocd_cmd(s, f'mdw 0x{addr:08X}')
    try:
        return int(r.split(':')[1].strip().split()[0], 16)
    except Exception:
        return None

def mww(s, addr, val):
    ocd_cmd(s, f'mww 0x{addr:08X} 0x{val:08X}')

# ── ADC battery read ──────────────────────────────────────────────────────────
def read_battery_raw(s):
    """Halt MCU, read ADC on PB0/ch8, resume. Returns 12-bit raw count."""
    ocd_cmd(s, 'halt')
    time.sleep(0.05)

    # Enable ADC clock and set prescaler
    ahb = mdw(s, AHBENR) or 0
    mww(s, AHBENR, ahb | (1 << 12))
    mww(s, RCC_CFGR2, 0x00003804)

    # Save PB0 mode, switch to analog (mode=3)
    moder = mdw(s, GPIOB_MODER) or 0
    mww(s, GPIOB_MODER, moder | (3 << (BAT_GPIO * 2)))
    time.sleep(0.15)  # let MOSFET gate cap charge via divider from 0V

    # Configure ADC: max sample time, channel 8, single conversion
    mww(s, ADC_SMPR2, 0x07000000)   # ch8 sample time = max (bits [26:24] = 7)
    mww(s, ADC_RSEQ1, 0x00000000)
    mww(s, ADC_RSEQ3, BAT_CHANNEL)
    mww(s, ADC_CTRL2, 0x00000001)                        # ADON
    mww(s, ADC_CTRL2, 0x00000001 | (7 << 17) | (1 << 20))  # EXTSEL=SW, EXTTRIG

    # Trigger and wait for EOC
    mww(s, ADC_STS, 0)
    mww(s, ADC_CTRL2, mdw(s, ADC_CTRL2) | (1 << 22))    # SWSTRRCH
    for _ in range(20):
        time.sleep(0.005)
        sts = mdw(s, ADC_STS) or 0
        if sts & 2:
            break

    raw = mdw(s, ADC_DAT) or 0
    raw = raw & 0xFFF

    # Restore PB0 mode
    mww(s, GPIOB_MODER, moder)

    ocd_cmd(s, 'resume')
    return raw

def raw_to_volts(raw):
    return raw * DIVIDER * VDDA / ADC_MAX

def volts_to_pct(v):
    """Rough LiPo SoC from voltage."""
    if v >= 4.20: return 100
    if v >= 4.06: return 90
    if v >= 3.98: return 80
    if v >= 3.90: return 70
    if v >= 3.83: return 60
    if v >= 3.75: return 50
    if v >= 3.71: return 40
    if v >= 3.67: return 30
    if v >= 3.61: return 20
    if v >= 3.49: return 10
    if v >= 3.27: return 5
    return 0

def bar(pct, width=20):
    filled = round(pct / 100 * width)
    return '[' + '#' * filled + '-' * (width - filled) + ']'

# ── GPIO status ───────────────────────────────────────────────────────────────
def read_gpio_status(s):
    # PB2 (STAT) is in analog mode in the charge firmware → IDR always reads 0.
    # Temporarily switch PB2 to digital input to get the real pin state.
    moder = mdw(s, GPIOB_MODER) or 0
    mww(s, GPIOB_MODER, moder & ~(3 << 4))  # PB2 = input (00)
    time.sleep(0.02)
    idr = mdw(s, GPIOB_IDR) or 0
    mww(s, GPIOB_MODER, moder)              # restore
    return {
        'coil':  (idr >> 0) & 1,
        'stat':  (idr >> 2) & 1,
        'bl':    (idr >> 4) & 1,
        'rst':   (idr >> 6) & 1,
    }

# ── Main ──────────────────────────────────────────────────────────────────────
def check_once(verbose=True):
    s = ocd_connect()
    try:
        ocd_cmd(s, 'halt')
        time.sleep(0.1)

        # Prefer the firmware mailbox — charge firmware reads ADC with proper
        # 100 ms settling delay so its values are more reliable than our halt reads.
        mb_raw   = mdw(s, MAILBOX_RAW)   or 0
        mb_count = mdw(s, MAILBOX_COUNT) or 0
        source = 'mailbox'

        if mb_raw > 50 and mb_count > 0:
            raw = mb_raw
        else:
            # Fallback: direct halt-and-read (less reliable due to settling)
            raw = read_battery_raw(s)
            source = 'direct'

        volts = raw_to_volts(raw)
        pct   = volts_to_pct(volts)
        gpio  = read_gpio_status(s)

        ocd_cmd(s, 'resume')
    finally:
        s.close()

    if raw < 50:
        print("ADC read failed or battery disconnected (raw={})".format(raw))
        return

    charging = gpio['stat'] == 0
    coil_ok  = gpio['coil'] == 0

    if verbose:
        ts = time.strftime('%H:%M:%S')
        print("[{}]  Battery: {:.3f}V  {}  {}%".format(ts, volts, bar(pct), pct))
        print("         ADC raw: {}  src={}  mb_count={}  |  "
              "Charging: {}  |  Coil: {}  |  BL: {}".format(
              raw, source, mb_count,
              'YES' if charging else 'no',
              'OFF' if coil_ok else 'ON (!!)',
              'off' if gpio['bl'] else 'on'))
        if not coil_ok:
            print("  !! WARNING: coil MOSFET gate is HIGH — heating element active !!")
        if volts < BAT_CRIT_V:
            print("  !! CRITICAL: {:.3f}V is below safe threshold ({:.1f}V)".format(volts, BAT_CRIT_V))
        elif volts < BAT_WARN_V:
            print("  !! LOW: {:.3f}V — charge soon".format(volts))
        print("         Note: divider ratio empirically ~0.71 (not the designed 1:28)")
    else:
        print("{:.3f}V ({}%)".format(volts, pct))

    return volts, pct

def main():
    p = argparse.ArgumentParser(description="Check vape battery voltage via SWD")
    p.add_argument('--watch', type=int, metavar='SEC',
                   help='Poll every SEC seconds (Ctrl-C to stop)')
    p.add_argument('-q', '--quiet', action='store_true',
                   help='One-line output only')
    args = p.parse_args()

    if args.watch:
        print(f"Polling every {args.watch}s — Ctrl-C to stop\n")
        try:
            while True:
                check_once(verbose=not args.quiet)
                print()
                time.sleep(args.watch)
        except KeyboardInterrupt:
            print("\nStopped.")
    else:
        check_once(verbose=not args.quiet)

if __name__ == '__main__':
    main()
